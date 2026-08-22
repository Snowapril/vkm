# Common Renderer Backend — Interface Contracts

This directory defines the abstract interfaces every backend must satisfy. Do not add platform-specific code here.

## Pure Virtual Methods — Must Be Overridden in Every Backend

### VkmDriverBase (`driver.h`)
```cpp
virtual bool initializeInner(const VkmEngineLaunchOptions* options) = 0;
virtual void destroyInner() = 0;
virtual VkmTexture* newTextureInner() = 0;
virtual VkmBuffer* newBufferInner() = 0;
virtual VkmStagingBuffer* newStagingBufferInner() = 0;
virtual VkmSampler* newSamplerInner() = 0;
virtual VkmTextureView* newTextureViewInner() = 0;
virtual VkmBufferView* newBufferViewInner() = 0;
virtual VkmSwapChainBase* newSwapChainInner() = 0;
virtual VkmCommandQueueBase* newCommandQueueInner() = 0;
virtual VkmPipelineStateBase* newPipelineStateInner() = 0;
```

### VkmSwapChainBase (`swapchain.h`)
```cpp
virtual bool createSwapChain(void* windowHandle) = 0;
virtual void destroySwapChain() = 0;
virtual VkmResourceHandle acquireNextImageInner() = 0;
virtual void presentInner() = 0;
```

### VkmCommandBufferBase (`command_buffer.h`)
```cpp
virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) = 0;
virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) = 0;
virtual void onEndRenderPass() = 0;
virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) = 0;
virtual void onUnbindPipeline() = 0;
// Backends without VkmDriverCapabilityFlags::TextureUpload (WebGPU) still override this,
// logging an error and recording nothing -- same shape as onCopyTexture.
virtual void onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture, uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer) = 0;
virtual void onSetDebugName(const char* name) = 0;
// Only when built with VKM_ENABLE_GPU_BREAD_CRUMBS (see "GPU Crash Handler" below):
virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) = 0;
virtual void onEndCommandBuffer() = 0;
```

### Resource upload: two paths, one entry point

`VkmDriverBase::uploadToTexture` and `uploadToBuffer` each pick between a staging-buffer copy
(allocate staging, record `copyBufferToTexture` / `copyBuffer`, submit, block) and a direct CPU
write into the destination's own memory (`VkmTexture::writeRegion` / `VkmBuffer::map()` +
`unmap()` -- no staging, no command buffer, no submit, no wait).

In both cases `VkmResourceUploadMode` only selects among what the destination already made
available; `isHostWritable()` is what decides. **Textures and buffers differ in who makes that
decision**, and deliberately so:

- **Textures: backend policy.** `VkmTexture::isHostWritable()` is inferred at creation -- a plain
  upload destination (`AllowTransferDst`, not an attachment or presentable) on a device that
  reports `VkmDriverCapabilityFlags::TextureHostCopy`. Callers cannot request or refuse it.
- **Buffers: caller opt-in.** `VkmBufferInfo::_accessHint` (`VkmMemoryAccessHint::HostWrite`) is
  the request; a buffer that does not ask stays device-local exactly as before. Inferring it would
  have silently moved every scene mega-buffer, ObjectData buffer and indirect-args buffer into
  host-visible memory, which is a bandwidth decision only the caller can make.

Either way the answer reports what was **allocated**, not what was asked for. On Vulkan both are
re-checked after the fact with `vmaGetAllocationMemoryProperties` -- a request is not a guarantee.
The device-level preconditions:

- `VkmDriverCapabilityFlags::TextureHostCopy` -- the backend has the mechanism (Metal
  `MTLStorageModeShared`; Vulkan `VK_EXT_host_image_copy`, since an OPTIMAL-tiled image cannot be
  memcpy'd into) **and** unified memory makes it worth using.
- A `HostWrite` buffer needs no capability flag on Vulkan/Metal (host-visible memory always
  exists), but it is unavailable on WebGPU: a `WGPUBuffer`'s usage flags fix its map mode for
  life and `MapWrite` only combines with `CopySrc`, so a buffer anything else touches can never
  be CPU-write-mapped. `isHostWritable()` stays false there and uploads keep going through staging.
- `HostWrite` always takes the *committed* allocation path. Both suballocation pools are backed by
  device-private memory (Vulkan's shared pool block, Metal's `MTLStorageModePrivate` heap), so a
  host-writable buffer cannot be placed in one; combining it with `VkmMemoryPlacementHint::Heap`
  warns. The same rule applies to a texture Metal placed in `MTLStorageModeShared`.

Both paths leave the resource GPU-readable, so callers never branch on which one ran. A backend
that offers neither needs no override; the base defaults are the unreachable-case guards.

### Transient (tile-memory-only) textures

`VkmResourceCreateInfo::Transient` asks for an attachment whose backing store never leaves
on-chip tile memory — Vulkan `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` behind a
`VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` image, Metal `MTLStorageModeMemoryless`. WebGPU has
no equivalent and warns.

Three rules make it safe, all enforced in common code so no backend can hand an illegal
descriptor to a validation layer:

1. **Sanitized at creation.** Vulkan VUID-`VkImageCreateInfo`-usage-00963/00966 and Metal's
   memoryless contract agree that such a resource may carry attachment usage and nothing else,
   and must carry at least one. `VkmDriverBase::newTexture` warns and clears the bit for any
   other combination — and for any buffer, since neither API has a transient buffer. Downgraded
   rather than rejected, matching how `ForcePooled` falls back.
2. **Pool type derived, never a separate field.** A surviving flag puts the handle in
   `VkmResourcePoolType::Transient`. Membership follows the *request*: a Vulkan device offering
   no lazily-allocated memory type still places its ordinary fallback allocation there.
3. **Load/store actions coerced.** `VkmCommandBufferBase::beginRenderPass` forces
   `VkmStoreAction::DontCare` and rewrites `VkmLoadAction::Load` on any attachment reporting
   `isTransient()`, logging an error for each. Storing one is a Metal validation error, and on
   Vulkan it forces the lazily-allocated memory to commit, undoing the request. The guard is
   keyed on the *grant*, and skipped entirely (one relaxed atomic load) until
   `VkmRenderResourcePool::hasTransientTextures()` latches.

`VkmTexture::isTransient()` reports what was allocated, the same request-vs-grant contract
`isHostWritable()` carries. A granted texture must never be sampled, blitted or read back.

### Aliased (memory-sharing) textures

`VkmResourceCreateInfo::Aliasable` lets two textures whose render-graph lifetimes do not overlap
share the same bytes — Vulkan binds both images into one `VkDeviceMemory` block at chosen
offsets, Metal creates both into one `MTLHeapTypePlacement` heap. WebGPU has neither and warns.

The packing lives in `VkmAliasedMemoryHeap` (`common/aliased_memory_heap.h`), which owns no
device memory at all: the driver supplies blocks through `onCreateAliasBlock`, and each backend
turns a `VkmAliasPlacement` into a real binding. That split is what makes the rules unit-testable
with no GPU (`tests/TestAliasedMemoryHeap.cpp`).

Four rules, all enforced in common code:

1. **Lifetimes are declared, never inferred.** Every subgraph touching an aliasable texture must
   call `VkmRenderSubGraph::addAliasedResource()`. A texture read through a bindless index or a
   `VkmResourceTable` is invisible to the graph, and `addReferencedResource` (which exists only
   to drive the deferred reclaimer) is opt-in and routinely incomplete. `compile()` catches the
   one case it can see — an aliasable texture attached without being declared — logs an error and
   **widens** the lifetime: an over-wide lifetime costs memory, an under-wide one corrupts pixels.
   A registered texture nobody declared this frame is treated as live for the whole graph, since
   an empty lifetime would let two such textures look mutually disjoint.
2. **Placement is decided once and never revisited.** `vkBindImageMemory` may be called once per
   image, `vkCreateImageView` needs bound memory, and `VkmResourceTableBase` bakes that view in —
   so moving a texture later would invalidate every table naming it. The consequence is a
   one-frame gate: an aliasable texture has no usable native handle until the frame after the
   first `compile()` that declares it. Check `isAliasPlaced()` before attaching it, building a
   table from it, or registering it bindless.
3. **Acquisition discards and fences, in one call.** `VkmRenderGraph::execute()` emits
   `VkmCommandBufferBase::acquireAliasedTexture()` before the first subgraph that declared each
   texture whose bytes are actually shared. Application code never calls it — forgetting it would
   be undefined pixels rather than a compile error. `compile()` also coerces that first use's
   load action to `DontCare`, since the previous contents belong to another texture.
4. **One global heap, because there is one queue.** A barrier's first synchronization scope covers
   everything earlier in *submission order* on the queue, which spans command buffers — so the
   acquisition barrier orders this frame's first write after the previous frame's reads of the
   other alias, and per-frame-slot heaps are unnecessary. **This holds only while everything
   submits to `(Graphics, 0)`.** A second queue would break it and need a timeline-semaphore wait
   instead; see `TODO.md`.

`isAliasable()` reports the grant, the same request-vs-grant contract `isHostWritable()` and
`isTransient()` carry. `Aliasable` is mutually exclusive with `Transient` (memoryless has no
memory to alias), `ExternalHandleOwner`, `DeferredCreation` and `AllowPresent`, requires at least
one attachment usage, and is texture-only — a buffer is never an attachment, so the undeclared-use
check could not exist for one.

### GPU virtual addresses

`VkmBuffer::getGPUVirtualAddress()` and `VkmStagingBuffer::getGPUVirtualAddress()` report the
buffer's address in the GPU's address space, or **0** where there is no such concept. Gated by
`VkmDriverCapabilityFlags::BufferDeviceAddress`:

- Metal: unconditional (`MTLBuffer.gpuAddress`) -- the argument buffers and the push-constant ring
  have bound by address since the MTL4 port.
- Vulkan: needs `VkPhysicalDeviceVulkan12Features::bufferDeviceAddress`. The driver's device
  creation already requests every supported feature, so nothing has to be enabled explicitly; what
  the capability gates is `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` on the allocator and
  `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` on every buffer (including the pool block, or its
  sub-allocations could not report one). A pooled buffer's address is the block's plus
  `getBufferOffset()`.
- WebGPU: no such concept exists; always 0.

### VkmBindlessResourceManagerBase (`bindless_resource_manager.h`)
```cpp
virtual uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) = 0;
virtual void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) = 0;
virtual uint32_t registerTexture(VkmResourceHandle textureHandle) = 0;  // UINT32_MAX on WebGPU
virtual void unregisterTexture(uint32_t slot) = 0;
```
`registerTexture` takes the **texture**, not a view: each backend publishes the texture's own
default view/native object, whose dimensionality already came from `VkmTextureInfo::_type`.
Each backend's `initialize()` also creates the one engine sampler published at
`kVkmBindlessSamplerBinding` -- created natively (`vkCreateSampler` /
`newSamplerStateWithDescriptor`), **not** via `VkmDriverBase::newSampler()`, because the
bindless manager is initialized from `initializeInner()`, before the render resource pool is
initialized and therefore before any pooled resource can exist.

### VkmPipelineStateBase (`pipeline_state_object.h`)
```cpp
virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) = 0;
virtual void destroyInner() = 0;
```

### VkmStagingBuffer (`staging_buffer.h`)
```cpp
virtual bool initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info) = 0;
virtual void* map() = 0;
virtual void unmap() = 0;
virtual void flush(uint64_t offset, uint64_t size) = 0;
virtual void writeDirect(uint64_t offset, const void* data, uint64_t size) = 0;
virtual uint64_t getGPUVirtualAddress() const;  // 0 without BufferDeviceAddress
```

### VkmBuffer (`buffer.h`)
```cpp
virtual bool initialize(VkmResourceHandle handle, const VkmBufferInfo& info) = 0;
virtual bool overrideExternalHandle(void* externalHandle) = 0;
// Non-pure: a backend that never reports isHostWritable() implements neither.
virtual void* map();                            // nullptr unless isHostWritable()
virtual void unmap();                           // the flush point; the pointer stays valid
virtual uint64_t getGPUVirtualAddress() const;  // 0 without BufferDeviceAddress
```
`map()` returns a pointer that lives as long as the buffer -- Vulkan keeps
`VMA_ALLOCATION_CREATE_MAPPED_BIT` allocations mapped and Metal's `Shared` `contents` is valid for
the buffer's lifetime, so `unmap()` exists only to flush (a no-op on Metal, `vmaFlushAllocation` on
Vulkan) rather than to tear a mapping down.

## Coordinate Space

Clip space is **+Y up** with a **0..1 depth range** — the HLSL/D3D convention the shaders are
authored in, and what Metal and WebGPU use natively. `GLM_FORCE_DEPTH_ZERO_TO_ONE` is defined
unconditionally in `base/platform.h` to match.

Metal and WebGPU need no compensation. Vulkan's NDC is +Y down, so it normalizes Y in the
toolchain: `vkm-compiler` builds Vulkan SPIR-V with DXC's `-fvk-invert-y` (Vulkan target only),
and the backend inverts its `VkmFrontFace` mapping in `toVkFrontFace` to cancel the winding flip
that the Y negation causes. A new backend whose NDC is +Y down must do the same; one that is
+Y up must do nothing.

`vkm-compiler` compiles SPIR-V **separately per backend** (`compileStage` → `compileToSpirv`,
one invocation per backend with a distinct `-D VKM_BACKEND_*`), so a Vulkan-only compiler flag
does not reach the Metal or WebGPU shaders. A Y-flip in *shared HLSL source* without a
per-backend `#if` guard, however, would follow the geometry into every backend — so guard any
source-level compensation on the backend define, or keep it in the per-backend compiler flags.

## Descriptor Set Convention

Sets are numbered by **update frequency**, so a set is rebound only as often as its contents
change. `frame_constants.h` declares the indices; `bindless_resource_manager.h` owns set 0's
layout.

| Set | Contents | Rewritten | Status |
|---|---|---|---|
| 0 | bindless resource arrays | never, after registration | implemented on all backends |
| 1 | per-frame camera constants (`VkmFrameConstants`) | once per frame | implemented on all backends |
| 2 | per-pass | — | reserved, not in any pipeline layout |
| 3 | per-draw | — | reserved, not in any pipeline layout |

Per-draw data that fits travels as push constants rather than through set 3 (see
`kVkmBindlessPushConstantSize`). Push constants are **vertex-stage only**; set 1 is readable
from every stage, which is the reason it exists.

Every pipeline layout declares sets 0 and 1 and every `bindPipeline` binds both, even for
shaders that reference neither — an unused-but-declared set is valid on all three backends, and
WebGPU in fact *requires* every declared bind group to be set before a draw. That is what keeps
the bind sites free of per-PSO knowledge.

Per-backend expression of set 1 (`VkmFrameConstantManager*`, reached via
`VkmDriverBase::getFrameConstantManager()`): Vulkan a `UNIFORM_BUFFER` descriptor set per frame
slot; WebGPU a bind group per frame slot at group 1; Metal a plain buffer binding at
`kVkmMetalFrameConstantBufferIndex`, because vkm-compiler declares set 1 *discrete*
(`add_discrete_descriptor_set`) so spirv-cross does not wrap it in a second argument buffer.

Each manager owns a raw native uniform buffer rather than a `VkmBuffer`: when they were written no
`VkmBuffer` could be host-writable (device-local on Vulkan, `StorageModePrivate` on Metal) and no
staging buffer can carry uniform usage (on WebGPU `MapWrite` only combines with `CopySrc`). This
mirrors what the bindless managers already do for their argument/mega-buffers, and has the same
consequence: these allocations bypass `newBuffer()`, so they do not appear in the memory tracker
and the Metal one registers itself into the Default residency set explicitly.
`VkmMemoryAccessHint::HostWrite` has since removed the first half of that reason on Vulkan and
Metal, so these managers could move onto `VkmBuffer` and become visible to the tracker; that
migration has not been done (logged in `TODO.md`).

The buffer holds `FRAME_COUNT` regions at `kVkmFrameConstantStride`. Writes are plain host
writes with no GPU synchronization, so `VkmEngine::render()` performs them only after that
frame slot's `VkmRenderGraph::ensureCompleted()`.

## Shader-Side Bindless Contract

`bindless_resource_manager.h` is the C++ side of the bindless set-0 layout;
`include/vkm/shaders/vkm_bindless.hlsli` is its shader-side mirror and the only place that
knows how each backend expresses bindless resources and push constants (real descriptor
indexing plus `[[vk::push_constant]]` on Vulkan/Metal, mega-buffers plus a slot table plus a
dynamic-offset UBO on WebGPU). Sample and engine HLSL declares its resources through that
header's macros — `VKM_PUSH_CONSTANTS`, `VKM_BINDLESS_VERTEX_PULLING`, `VKM_LOAD_INDEX`,
`VKM_LOAD_VERTEX`, plus `VKM_BINDLESS_TEXTURE_CUBE_ARRAY` / `VKM_BINDLESS_TEXTURE_2D_ARRAY`
and `VKM_BINDLESS_SAMPLER` for sampled textures — and must not test `VKM_BACKEND_*` itself.
The texture/sampler macros exist only in the non-WebGPU branch (WGSL has no runtime-sized
texture arrays), so a shader that needs them is excluded from WebGPU builds in CMake rather
than failing at shader-compile time. Changing a binding number or
the slot-table layout means editing the header and `bindless_resource_manager.h` together,
not every shader.

`buildscripts/ShaderCompile.cmake` passes `include/vkm/shaders` to `vkm-compiler` as
`--include-dir` (which becomes dxc's `-I`) and adds every `.hlsli` there to each shader
command's `DEPENDS`; dxc emits no depfile, so that list is what makes a header edit rebuild
the shader caches.

## Constants

- `FRAME_BUFFER_COUNT = 3` — triple-buffering, fixed. Do not parameterize.
- `BACK_BUFFER_COUNT = 3` — same value, swapchain back buffers.
- `VKM_INVALID_RESOURCE_HANDLE` — sentinel for invalid handles; check `handle.isValid()` before use.

## VkmResourceHandle Rules

```cpp
struct VkmResourceHandle { uint64_t id; VkmResourcePoolType poolType; VkmResourceType type; uint32_t generation = 0; };
```

- Allocated by `VkmRenderResourcePool` — do not construct raw handles manually.
- `id == (uint64_t)-1` means invalid. Use `handle.isValid()`.
- `VkmResourceType`: Texture=0, Buffer=1, StagingBuffer=2, Sampler=3, TextureView=4, BufferView=5.
- Pooled resources: `handle.isPooledResource()` true when `poolType != Undefined`. This is about
  the *render resource pool* (handle ownership) and has nothing to do with
  `VkmMemoryPlacementHint::Heap` (memory suballocation) — the collision is why that enumerator is
  spelled `Heap` rather than `Pooled`.
- `VkmResourcePoolType`: Default=0, Transient=1. Every sub-pool has its own per-type id space,
  so two handles can share an `id` and still differ. `Transient` holds textures only, and
  membership follows the *request* — see "Transient (tile-memory-only) textures" below.
- `generation` is bumped by the pool each time a slot is released; `getResource()` only
  returns a resource when the handle's `generation` still matches the slot's, giving views a
  weak-reference liveness check. `allocateResourceLocked()` recycles released ids from a
  per-type free-list before growing the pool, so a stale (pre-release) handle sharing a
  recycled slot's `id` is correctly rejected by its now-mismatched `generation`.
  `isValid()`/`isPooledResource()` stay id/poolType-based only.

## View Handles

`TextureView`/`BufferView` reuse `VkmResourceHandle`/`VkmRenderResourcePool` directly — there is
no separate `VkmViewHandle` type. A view's `initialize()` resolves its parent resource via
`driver->getRenderResourcePool()->getResource<VkmXxxBackend>(info._texture` or `_buffer)`, so the
parent must already be a live, successfully-initialized resource in the same pool before the view
is created. Render-graph recording code resolves a view handle to a native handle the same way:
`pool->getResource<VkmTextureViewVulkan>(handle)->getImageView()`. Buffer views have no native
object on Metal/WebGPU (no buffer-view concept in either API) — only Vulkan creates a real
`VkBufferView`, and only when a texel format is requested; regular UBO/SSBO bindings use the
view's `getOffset()`/`getSize()` metadata directly instead.

## View Creation Ownership

Views are created **only** via `texture->createView(info)` / `buffer->createView(info)`.
`VkmDriverBase::newTextureView()` / `newBufferView()` are `protected` and friended to
`VkmTexture` / `VkmBuffer` respectively — they are not directly callable from application or
render-graph code. `createView()` always overwrites the parent-handle field
(`VkmTextureViewInfo::_texture` / `VkmBufferViewInfo::_buffer`) with the creating resource's
own handle, ignoring whatever the caller set, and records the resulting view handle so the
parent owns it (see Deferred Destruction). This guarantees a view can never reference a parent
other than the one it was created from, and that every view is tracked for cascading release.

## Per-Resource GPU Usage Tracking

`VkmRenderResource` (`render_resource.h`) tracks the last timeline value it was used with, keyed
by the timeline's own identity, in a `std::vector<VkmGpuEventTimelineObject>` (each
`VkmCommandQueueBase` owns exactly one `VkmGpuEventTimelineBase`, so the timeline pointer already
uniquely identifies the queue instance — no separate queue-type/index parameter is needed):
```cpp
void recordUsage(VkmGpuEventTimelineObject timelineObject);
VkmGpuEventTimelineObject getLastUsage(VkmGpuEventTimelineBase* queueTimeline) const;
const std::vector<VkmGpuEventTimelineObject>& getAllUsages() const;
bool hasAnyPendingUsage() const; // non-blocking poll via queryLastCompletedTimeline()
```
Only the latest usage per queue instance is kept — an earlier submit on the same queue is
implied complete once a later one is. `VkmRenderSubGraph::addReferencedResource(VkmResourceHandle)`
lets descriptor-binding recording code declare which resources a subgraph touches;
`VkmRenderGraph::execute()` calls `recordUsage()` for each after the subgraph's commands are
submitted, tagged with that submit's `VkmGpuEventTimelineObject`. `execute()` currently only
ever submits to the `Graphics` queue (hardcoded), so all recorded usage is tagged with the
graphics queue's timeline until the render graph dispatches to multiple queue instances.

## Deferred Destruction

Never call `VkmRenderResourcePool::releaseResource()` directly on a resource that may have
been GPU-used — it destroys the resource's native handle **immediately and synchronously**,
with no regard for in-flight GPU work. Once a resource has (or may have) recorded usage via
`recordUsage()`, route its destruction through `VkmDriverBase::getDeferredReclaimer()->requestRelease(handle)`
instead. `VkmDeferredResourceReclaimer` (`deferred_resource_reclaimer.h`) snapshots the
resource's per-queue recorded usage, and only calls the real `releaseResource()` once every
one of those timelines has completed (checked via the non-blocking `queryLastCompletedTimeline()`,
never a blocking `waitIdle()` — that would defeat the purpose).

**Cascading release.** `VkmRenderResource::getOwnedChildHandles()` (default empty; overridden by
`VkmTexture` / `VkmBuffer` to return their created views) declares child resources a parent owns.
When a parent is `requestRelease()`d, the reclaimer first recursively requests release of every
still-live child, then queues the parent's own entry. The parent's entry does not become ready
until **every** declared child is gone from the pool AND the parent's own recorded GPU usage has
completed — so a view is always reclaimed before the texture/buffer it references, and neither is
reclaimed while GPU work is still in flight.

A dedicated background thread (mutex + condition_variable, woken on new requests or a ~4ms
periodic timeout) drives this on Vulkan/Metal/WebGPU-desktop-hypothetical platforms; it is
owned by `VkmDriverBase`, started at the end of `initialize()`, stopped as the first step of
`destroy()` (draining any still-pending entries with a bounded blocking `waitIdle()` — the
one place blocking is acceptable, since this only runs at shutdown). **On WASM**, which
cannot spawn a blocking background thread, `start()` is a no-op; the same non-blocking sweep
is instead driven once per frame via `pollOnce()`, called unconditionally from
`VkmRenderGraph::execute()` right after the frame's submit (a harmless redundant extra check
on the other backends, where the real thread already does the work).

## Per-Resource Memory Tagging

`VkmResourceMemoryTag` / `VkmResourceCategoryUsage` (`renderer_common.h`) mirror
`vkm::MemoryTracker`'s CPU-side tag pattern for individual GPU resource allocations:
```cpp
struct VkmResourceMemoryTag { uint64_t requestedSize, allocatedSize; uint32_t alignment;
                             std::string name, metadata; VkmResourceType type; };
struct VkmResourceCategoryUsage { uint64_t totalRequestedBytes, totalAllocatedBytes; uint32_t liveCount; };
```
`VkmDriverBase::newXxx()` tags each successfully-initialized resource via
`VkmRenderResourcePool::tagResource(handle, tag)` right after `initialize()`. Query with:
```cpp
std::optional<VkmResourceMemoryTag> getResourceMemoryTag(VkmResourceHandle) const;
VkmResourceCategoryUsage getCategoryMemoryUsage(VkmResourceType) const;
VkmResourceCategoryUsage getTotalMemoryUsage() const;
std::vector<VkmResourceMemoryTag> getAllMemoryTags() const;
```
**Semantics split:** a per-handle tag goes empty (`getResourceMemoryTag()` returns `nullopt`)
the moment its handle is released; the per-category totals *persist and decrement* on release
rather than resetting — they are the meaningful historical/debugging signal. A tag's `name`
comes from `VkmResourceInfo::_debugName` (copied into a `std::string`; the raw pointer is never
retained).

`VkmRenderResource::getAllocatedSize()` / `getMemoryAlignment()` (`render_resource.h`) feed a
tag's `allocatedSize`/`alignment`:
- **Vulkan** = real numbers (`VmaAllocationInfo::size` + `vkGetBuffer/ImageMemoryRequirements`);
  a `Heap` buffer sub-range reports its requested size and locally-computed alignment (no
  distinct VMA allocation to introspect).
- **Metal / WebGPU** = best-effort passthrough of the requested size (no allocation-introspection
  API exists on either), with a `256`-byte conventional alignment. Metal textures are the
  exception: `heapTextureSizeAndAlignWithDescriptor:` gives a real alignment, and the `256`
  default survives only when that call reports no heap footprint.
- **Sampler / TextureView / BufferView** = always `0` (no independent GPU memory allocation of
  their own), via the base-class default — no override.
- **A granted transient texture** = `allocatedSize` `0` on both Vulkan and Metal, with the tag's
  `metadata` set to `"transient"`. Nothing is committed, so reporting the image's virtual size
  would inflate the report by an attachment that costs no device memory; the `metadata` string
  is what tells that apart from an untagged `0`. `requestedSize` is unchanged, which is what
  makes the saving visible.

### Device-reported memory (the "actual" side)

Everything above is what the engine *asked for*. `VkmDriverBase::getGpuMemoryStats()` is the
counterpart the API itself reports:
```cpp
struct VkmGpuMemoryStats { uint64_t _deviceAllocatedBytes, _deviceBudgetBytes,
                           _poolReservedBytes, _poolUsedBytes;
                           bool _hasDeviceStats, _hasPoolStats; };
```
- **Metal** = `[MTLDevice currentAllocatedSize]` / `recommendedMaxWorkingSetSize`, plus each
  heap pool block's `MTLHeap.currentAllocatedSize` (reserved) vs `usedSize` (used).
- **Vulkan** = `vmaGetHeapBudgets()` summed over device-local heaps (host-visible heaps are
  skipped so they don't double-count against process RSS), plus `vmaCalculateStatistics()`'s
  `blockBytes` vs `allocationBytes`.
- **WebGPU** = the base-class default: both flags `false`. WebGPU exposes no memory
  introspection, and echoing the engine's tracked numbers back would misrepresent them as
  measured.

`vkm::captureMemorySnapshot()` (`renderer/memory_report.h`) joins this with the CPU-side
`MemoryTracker` and the OS's `getProcessMemoryStats()` into the sample the ImGui Memory
Inspector (F8) and the shutdown log dump both render.

## Debug Naming (GPU Capture)

`VkmEngineLaunchOptions::enableGpuCapture` (parsed from the `--enable-gpu-capture` CLI flag,
alongside the existing `--enable-validation-layer`) opts a run into GPU-capture tooling support.
`VkmDriverBase::isDebugNamingEnabled()` returns `enableValidationLayer || enableGpuCapture` — but
is always `false` when `initialize()` was called with a null options pointer (some test fixtures
do this, having opted out of the whole launch-configuration story). It is computed once in
`initialize()` and cached.

When `isDebugNamingEnabled()` is true AND a `VkmResourceInfo::_debugName` was supplied, each of
`driver.cpp`'s 6 orchestration methods (`newTexture`/`newBuffer`/`newStagingBuffer`/`newSampler`/
`newTextureView`/`newBufferView`) calls the resource's `setDebugName()` right after `tagResource()`;
`VkmCommandQueueBase::initialize()` likewise gates its `setDebugName(_queueName)` on the same flag.
No `setDebugName()` method body itself changed — gating is entirely at these call sites.

### Programmatic frame capture (Metal)

Compile-time gate: `VKM_GPU_CAPTURE` (CMake option `GPU_CAPTURE`, defaulting ON except in
Release builds; pass `-DGPU_CAPTURE=ON` to opt a Release build back in — same pattern as
`GPU_BREAD_CRUMBS`). Everything below, including the `VkmDriverBase`
`onFrameBegin()/onFrameEnd()/requestGpuFrameCapture()` hooks and the `MTL_CAPTURE_ENABLED`
env setup, compiles away when the gate is off.

When `enableGpuCapture` is set, `VkmDriverMetal::postInitializeInner()` additionally creates a
frame-aligned `MTLCaptureScope` (label "vkm frame") on the Graphics MTL4 queue via
`newCaptureScopeWithMTL4CommandQueue:` and installs it as `MTLCaptureManager.defaultCaptureScope`.
`VkmEngine::loopInner()` brackets every frame with the cross-backend
`VkmDriverBase::onFrameBegin()/onFrameEnd()` hooks (no-ops on Vulkan/WebGPU); the Metal overrides
begin/end that scope, so Xcode's Metal capture button records a bounded frame instead of being
unavailable for the MTL4 workload.

One-shot `.gputrace` export: `VkmDriverBase::requestGpuFrameCapture(startFrameDelay, frameCount)`
(F9 in the ImGui overlay, or `--gpu-capture-frame` to arm at startup — that flag implies
`--enable-gpu-capture`) arms a capture consumed at a subsequent `onFrameBegin()`, writing
`vkm_capture_<timestamp>.gputrace` to the working directory via
`MTLCaptureDestinationGPUTraceDocument`. `--gpu-capture-start-frame N` (default 0) delays the
capture start by N frames and `--gpu-capture-frame-count N` (default 1) records N consecutive
frames into the trace; both apply to F9-triggered captures too. `MTL_CAPTURE_ENABLED=1` is set automatically before Metal
device creation when either capture flag appears in the raw process arguments (see
`vkmCreateSystemDefaultDevice()` in `platform/apple/application.mm`); if capture is still reported
unsupported, launch with `MTL_CAPTURE_ENABLED=1` set in the shell.

Xcode workflow: configure with `cmake -G Xcode` (the root CMakeLists already special-cases the
nested DXC build for the Xcode generator), open the generated project, add `--enable-gpu-capture`
to the sample scheme's arguments, and use the Metal capture button — the capture dialog defaults
to the "vkm frame" scope. Traces of Metal 4 workloads require Xcode 26+ to record and open.

## GPU Crash Handler

Two independent gates apply:
- **Compile-time**: `VKM_ENABLE_GPU_BREAD_CRUMBS` (CMake option `GPU_BREAD_CRUMBS`, defaulting
  ON except in Release builds; pass `-DGPU_BREAD_CRUMBS=ON` to opt a Release build back in)
  scopes ALL breadcrumb instrumentation -- the breadcrumb ring, the per-subgraph marker
  machinery below, the `onWriteCompletionMarker`/`onEndCommandBuffer` pure virtuals, and their
  call sites. Device-loss **detection and logging are deliberately outside the macro**: every
  build configuration still detects a crash and logs its error code/reason via
  `reportCrash()`; without the macro the report just notes that breadcrumb history was
  compiled out.
- **Runtime**: `VkmEngineLaunchOptions::enableGpuCrashDump` (parsed from
  `--enable-gpu-crash-dump`) opts a run into breadcrumb recording.
  `VkmDriverBase::isGpuCrashDumpEnabled()` gates `VkmGpuCrashHandler::recordSubmission()`'s
  bookkeeping cost and (on Vulkan) `VK_EXT_device_fault` extension enablement; it is computed
  once in `initialize()`, alongside `_debugNamingEnabled`, before `initializeInner()` runs.
  The device-fault gate stays live even without the compile-time macro.

`VkmGpuCrashHandler` (`gpu_crash_handler.h`, owned one-per-driver via
`VkmDriverBase::getGpuCrashHandler()`) has two entry points every backend calls:
```cpp
void recordSubmission(VkmCommandQueueBase* queue, const CommandSubmitInfo& submitInfo, VkmGpuEventTimelineObject timelineObject);
void reportCrash(const char* backendName, const std::string& errorCode, const std::string& reason);
```
`recordSubmission()` is called from each backend's `VkmCommandQueueBase::submit()` override,
right after determining the timeline object for that submission and before the native submit
call. It is a no-op unless `isGpuCrashDumpEnabled()`; otherwise it appends a bounded breadcrumb
(oldest evicted past `MAX_BREADCRUMB_ENTRIES`) recording the queue name and each submitted
command buffer's `VkmCommandBufferBase::getDebugName()` (an in-engine-only bookkeeping name,
distinct from `VkmDriverResourceBase::setDebugName()` -- never pushed to a native driver API;
unnamed command buffers get an auto `"<queueName>#<index>"` fallback).

`reportCrash()` is called once a backend detects a device-lost/GPU-error condition: Vulkan's
`vkCheckResult()` on `VK_ERROR_DEVICE_LOST` (see `vulkan/vulkan_util.cpp`), Metal's MTL4 commit
feedback handler on a non-nil `MTL4CommitFeedback.error`, WebGPU's `deviceLostCallbackInfo` on
any reason other than `WGPUDeviceLostReason_Destroyed` (that reason also fires on ordinary
engine teardown, not a crash). It always logs the error code/reason regardless of the gating
flag, then walks any recorded breadcrumbs newest-first, classifying each as `COMPLETED` (its
timeline value was already reached, via a non-blocking `queryLastCompletedTimeline()` poll) or
`SUSPECT` (not yet completed -- may be the faulting submission, or simply still in flight).

## Per-Subgraph GPU Completion Markers

Everything in this section exists only under `VKM_ENABLE_GPU_BREAD_CRUMBS` (see the
compile-time gate above).

Whole-submission `COMPLETED`/`SUSPECT` (above) can't tell which of the (possibly many)
subgraphs recorded into one frame's single command buffer actually ran before a crash.
`VkmGpuCrashHandler` additionally owns a persistent marker buffer
(`FRAME_COUNT * MAX_SUBGRAPHS_PER_FRAME` `uint32_t` slots, `MAX_SUBGRAPHS_PER_FRAME = 128`) and
a small constant-`1` buffer, both lazily created via `ensureMarkerBuffersCreated()` the first
time `isGpuCrashDumpEnabled()` is true and a marker/one-buffer handle is requested.

`VkmCommandBufferBase::writeCompletionMarker(markerBuffer, oneBuffer, subGraphId, offset)`
copies 4 bytes from `oneBuffer` to `markerBuffer` at `offset` (`onWriteCompletionMarker()`,
backend-specific -- see below) and records `subGraphId` into
`getRecordedSubGraphIds()`. `VkmRenderGraph::execute()` calls it right after each
`subGraph->commit(commandBuffer)` returns, using `VkmGpuCrashHandler::getMarkerOffset(frameIndex,
subGraphId)` for the offset -- safe to call there since no backend ever leaves a render/compute
encoder open across a subgraph's `commit()` boundary. Before a frame's subgraphs are recorded,
`execute()` calls `VkmGpuCrashHandler::clearFrameMarkers(frameIndex)`, which blocks (there is no
other reliable "this frame slot's prior GPU work is done" signal in the live render loop --
`VkmEngine::prepareRender()` is dead code and `VkmRenderGraphCommitOptions::waitForCompletion` is
currently a no-op) then zeroes that frame's slice. `recordSubmission()` copies
`getRecordedSubGraphIds()` plus the submission's `CommandSubmitInfo::frameIndex` into the
breadcrumb; `reportCrash()` reads the marker buffer for each breadcrumb's frame index and prints
`COMPLETED`/`NOT COMPLETED` per recorded subgraph.

**Why "copy from a constant-1 buffer" instead of a native fill command, on every backend:**
Metal4 has no blit encoder at all (`MTL4CommandBuffer` only exposes
`renderCommandEncoderWithDescriptor:`/`computeCommandEncoder`/`machineLearningCommandEncoder`);
buffer copy/fill lives on `MTL4ComputeCommandEncoder`, and its `fillBuffer:range:value:` only
repeats a single **byte** across the range (byte `1` repeated = `0x01010101`, not an exact
`0x00000001`). A `copyFromBuffer:` from a pre-populated constant buffer is the only way to get
an exact `1`, so all three backends use the same "copy 4 bytes" strategy for consistency
(Vulkan: `vkCmdCopyBuffer`; Metal: `copyFromBuffer:` on a compute encoder; WebGPU:
`wgpuCommandEncoderCopyBufferToBuffer`).

**Metal batches its marker writes.** Unlike Vulkan/WebGPU (whose `onWriteCompletionMarker()`
records its copy immediately -- no dedicated encoder needed), Metal's
`onWriteCompletionMarker()` only queues `(markerBuffer, oneBuffer, offset)` into
`_pendingMarkerWrites`; `onEndCommandBuffer()` (a pure virtual called from
`VkmCommandBufferBase::endCommandBuffer()`, no-op on Vulkan/WebGPU) flushes all of a command
buffer's queued writes as a *single* compute pass. Opening/closing a separate compute encoder
per subgraph was observed, under real interactive use, to cause progressively worsening
`MTL4CommandQueueErrorTimeout` and an eventual command-queue stall -- batching into one pass
per command buffer resolved it.

**WebGPU-specific buffer-mapping constraint.** A WebGPU buffer's usage flags fix which map mode
it may *ever* use (`MapRead` only combines with `CopyDst`; `MapWrite` only with `CopySrc`) --
a single buffer can never be both CPU-write-mappable and CPU-read-mappable, unlike Vulkan/Metal
where the marker/one buffers are simple persistently-mapped host-coherent memory. Both buffers
are therefore created `MapRead | CopyDst` (or `AllowTransferSrc`'s equivalent for the one
buffer) and kept **unmapped** during normal frame recording -- a WebGPU buffer must be unmapped
for the GPU to access it at all. `VkmStagingBuffer::writeDirect(offset, data, size)` (a new pure
virtual) lets the CPU update a buffer without requiring it to be mapped:
Vulkan/Metal implement it as `memcpy` into the always-mapped pointer (+ `flush()` on Vulkan);
WebGPU implements it via `wgpuQueueWriteBuffer()`, which per spec takes effect before the next
`wgpuQueueSubmit()` regardless of the buffer's current map state. `clearFrameMarkers()` uses
`writeDirect()` for this reason; `reportCrash()`'s one-time crash-time readback uses `map()`
instead (cheap/synchronous on Vulkan/Metal; a real async `wgpuBufferMapAsync(Read)` round trip
on WebGPU, best-effort since the device may already be unusable by then).

## Per-Subgraph GPU Timestamps

`VkmGpuProfiler` (`common/gpu_profiler.h`, owned by `VkmDriverBase` alongside
`VkmGpuCrashHandler`) times each render graph subgraph on the GPU and groups the results by the
command queue they ran on. Unlike the completion markers above, this is **not** gated on a
compile-time flag: recording is always on wherever the device supports timestamp queries,
because the debug overlay's always-visible "GPU: x.xx ms" stat reads the same collector.
`isCapturing()` only decides whether resolved frames are kept in the 240-frame history ring that
`VkmGpuProfilerInspector` (F6) draws.

**Slot model.** The driver is asked for one flat pool of `kTimestampSlotCount` timestamp slots.
The profiler partitions it into `kMaxPendingSubmissions` fixed buckets of
`2 * kMaxZonesPerSubmission` slots, handed out and retired in submission order, so allocation is
a bucket index rather than interval arithmetic and `collect()` can stop at the first bucket the
GPU has not finished. Zone `i` of a submission owns slots `2i` (begin) and `2i+1` (end).

**Recording.** `VkmRenderGraph::execute()` calls `beginSubmission(queue, cb, 1 + subGraphCount)`
right after `beginCommandBuffer()`, wraps the whole submission in a depth-0 `"Frame"` zone, and
each subgraph in a depth-1 zone named after it (interned via `VkmCpuProfiler::internName`, so a
GPU zone's name outlives the frame that recorded it -- it is only read once the GPU is done).
`beginSubmission` records the backend's reset for **exactly** the `2 * zoneCount` slots that will
be written: a Vulkan slot that is reset but never written stays permanently unavailable to
`vkGetQueryPoolResults` (`VUID-vkGetQueryPoolResults-None-09401`), so nothing may be reserved
that is not written.

**Resolution.** `endSubmission()` hands over the submit's `VkmGpuEventTimelineObject`;
`VkmEngine::loopInner()` calls `collect()` once per frame, which resolves only submissions whose
timeline has already completed (the same non-blocking `queryLastCompletedTimeline()` poll the
breadcrumbs use) and therefore never stalls.

**The command-buffer seam** is `beginGpuZone(beginSlot, endSlot)` / `endGpuZone()` /
`resolveGpuZones(firstSlot, count)`, all empty-default virtuals. Both slots are handed over at
*begin* time purely for WebGPU's sake (see below). Closing the outermost zone is what triggers
`resolveGpuZones()`, so it must happen before `endCommandBuffer()`.

| Backend | Pool | Write | Resolve |
|---|---|---|---|
| Vulkan | `VkQueryPool` of `VK_QUERY_TYPE_TIMESTAMP` | `vkCmdWriteTimestamp2` at `TOP_OF_PIPE` / `BOTTOM_OF_PIPE`; legal inside a render pass | `vkGetQueryPoolResults`, masked to the graphics family's `timestampValidBits` |
| Metal | `MTL4CounterHeap` (`MTL4CounterHeapTypeTimestamp`) | `[MTL4CommandBuffer writeTimestampIntoHeap:atIndex:]` -- **command-buffer** scope, like `pushDebugGroup`, so no encoder is split | `[heap resolveCounterRange:]` on the CPU timeline |
| WebGPU | `WGPUQuerySet` + a `QueryResolve` buffer and a `MapRead` copy | pass descriptor `timestampWrites` only | `wgpuCommandEncoderResolveQuerySet` + buffer-to-buffer copy, recorded in the same command buffer; blocking `wgpuBufferMapAsync` afterwards |

**Why `endGpuZone()` returns a bool.** WebGPU has no encoder-level timestamp write at all: a
begin/end pair can only be carried by one render or compute pass descriptor
(`beginningOfPassWriteIndex` / `endOfPassWriteIndex`), which must be filled *before* the pass is
opened -- hence both slots arriving at `beginGpuZone`. `VkmCommandBufferWebGPU` keeps a stack of
open zones and gives each new pass the innermost not-yet-attached one, so a subgraph's zone wins
over the submission-wide zone around it. A zone that enclosed no pass (a transfer subgraph, or
that outer zone) is therefore never written, and `endGpuZone()` returning false is how the
profiler learns to drop it rather than report a span it never measured. Vulkan and Metal always
return true. Consequences are recorded in `TODO.md`.

`getGpuTimestampPeriodNs()` converts raw ticks to nanoseconds: Vulkan's
`VkPhysicalDeviceLimits::timestampPeriod`, `1e9 / [MTLDevice queryTimestampFrequency]` on Metal,
`1.0` on WebGPU (the spec defines its timestamps as nanoseconds).

## Temporal Upscaling (`upscaler.h`)

`VkmUpscalerBase` is the optional temporal-upscaler abstraction: render-extent color/depth/motion
in, display-extent image out, with vendor-managed history in between (MetalFX on Metal, FSR on
Vulkan builds that include the FidelityFX SDK; WebGPU has none).

- **Capability-gated, not pure virtual.** `VkmDriverBase::newUpscalerInner()` is non-pure and
  returns null by default; only a backend that sets `VkmDriverCapabilityFlags::TemporalUpscaling`
  in `initializeInner()` overrides it. `newUpscaler()` checks the flag and logs, so callers check
  the flag where absence is expected (the `newAccelerationStructure` shape).
- **Caller-owned lifetime.** Same contract as `newResourceTable`: `destroy()` then `delete`, only
  once no in-flight frame can still reference it. Extents are fixed at creation — a resize **or a
  mode change** retires the old upscaler (FRAME_COUNT-frame delay) and creates a new one.
- **The engine owns the preset, not the app.** `VkmUpscaleMode` (Off, Native, Quality, Balanced,
  Performance) lives beside the jitter helpers in `upscaler.h`, and `VkmEngine` holds the live
  value: `getUpscaleMode()`, `setUpscaleMode()`, and `getRenderExtent()`, which is the single
  rounding rule the engine's camera viewport and an app's render targets both follow.
  **Native AA is the default wherever the driver reports `TemporalUpscaling` and the app returns
  true from `AppDelegate::consumesUpscaleMode()`** — at ratio 1 the render extent equals the
  display extent and the upscaler is anti-aliasing, vkm's only AA. **F2** cycles the presets and
  `--gv_upscale_mode=N` seeds one. An app that consumes the mode must include it in whatever test
  decides to rebuild its targets: Off and Native share a render extent, so extents alone cannot
  tell them apart.
- **Render-graph integration.** `recordDispatch()` appends one compute subgraph that declares
  color/depth/motion as `ShaderSampledRead` and the output as `ShaderStorageReadWrite`; the
  backend encode runs inside the subgraph callback against barriers the ordinary plan placed.
  The callback records through the backend's own library (MetalFX/FSR), not a vkm PSO.
- **Conventions.** Motion is the G-buffer's UV-space current→previous `.xy` (extra channels are
  ignored); jitter is pixels, +x right / +y down, the same value handed to
  `VkmCamera::setJitterPixels`; depth is non-inverted `[0,1]` (`glm::perspectiveRH_ZO`).

## VkmCommandQueueType

```cpp
enum class VkmCommandQueueType : uint8_t { Graphics=0, Compute=1, Transfer=2, Count=3 };
```
`VkmDriverBase::_commandQueues` is indexed by `(uint8_t)queueType`.

## Adding a New Backend

1. Derive `VkmDriver<Name>` from `VkmDriverBase`, override all 6 pure virtuals.
2. Derive `VkmSwapChain<Name>` from `VkmSwapChainBase`, override all 4 pure virtuals.
3. Derive `VkmCommandBuffer<Name>` from `VkmCommandBufferBase`, override all pure virtuals
   (unconditional ones include `onDraw`/`onDrawIndirectCount`/`onDispatch`/
   `onResourceBarrier`/`onSetPushConstants`, plus
   `onWriteCompletionMarker`/`onEndCommandBuffer` under `VKM_ENABLE_GPU_BREAD_CRUMBS`).
   Note the compute lifecycle contract: a compute pass is opened by `onBindPipeline` for a
   compute pipeline and closed by `onUnbindPipeline`, so `onDispatch` may assume an encoder
   exists. `onResourceBarrier` may be a documented no-op where the backend's own pass
   boundaries already establish the ordering (WebGPU).
   `onDrawIndirectCount` carries a `VkmIndirectArgumentLayout`; take the record stride from
   `vkmGetIndirectArgumentStride(layout)` rather than assuming a struct size. The common layer
   forwards only `VkmIndirectArgumentLayout::NonIndexed` today (it rejects the rest, since the
   engine has no bound index buffer), so a backend needs no branch on the layout.
4. Derive `VkmPipelineState<Name>` from `VkmPipelineStateBase`, override both pure virtuals (`createInner`/`destroyInner`), and return it from `VkmDriver<Name>::newPipelineStateInner()`.
5. Add CMake flag `VKM_USE_<NAME>_API` and guard source inclusion in `src/vkm/CMakeLists.txt`.
6. Do not modify any existing method signatures in this directory.
7. Wire a device-lost/GPU-error detection path that calls `reportCrash()` (always compiled),
   and call `VkmGpuCrashHandler::recordSubmission()` in the new
   `VkmCommandQueueBase::submit()` override inside `#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)`
   (see "GPU Crash Handler" above).

## Code Review Checklist for Common Interface Changes

- [ ] No new pure virtual methods without implementing them in ALL existing backends (Vulkan + Metal + WebGPU)
- [ ] `FRAME_BUFFER_COUNT` not changed
- [ ] `VkmResourceHandle` layout changes (e.g. the `generation` field added for view weak-references) must update BOTH `operator==`/`!=` AND the `std::hash` specialization together — they are not auto-derived
- [ ] Public method signatures in base classes unchanged (breaking ABI)
