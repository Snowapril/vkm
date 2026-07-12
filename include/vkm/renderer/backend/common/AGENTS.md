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
```

### VkmPipelineStateBase (`pipeline_state_object.h`)
```cpp
virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) = 0;
virtual void destroyInner() = 0;
```

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
- Pooled resources: `handle.isPooledResource()` true when `poolType != Undefined`.
- `generation` is bumped by the pool each time a slot is released; `getResource()` only
  returns a resource when the handle's `generation` still matches the slot's, giving views a
  weak-reference liveness check. Purely additive scaffolding — not exercised today since the
  pool never reuses an `id`, so a released slot's generation is never compared against a fresh
  handle in the same slot. `isValid()`/`isPooledResource()` stay id/poolType-based only.

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
  a pooled buffer sub-range reports its requested size and locally-computed alignment (no
  distinct VMA allocation to introspect).
- **Metal / WebGPU** = best-effort passthrough of the requested size (no allocation-introspection
  API exists on either), with a `256`-byte conventional alignment.
- **Sampler / TextureView / BufferView** = always `0` (no independent GPU memory allocation of
  their own), via the base-class default — no override.

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

## GPU Crash Handler

`VkmEngineLaunchOptions::enableGpuCrashDump` (parsed from `--enable-gpu-crash-dump`) opts a run
into breadcrumb recording. `VkmDriverBase::isGpuCrashDumpEnabled()` gates
`VkmGpuCrashHandler::recordSubmission()`'s bookkeeping cost and (on Vulkan)
`VK_EXT_device_fault` extension enablement; it is computed once in `initialize()`, alongside
`_debugNamingEnabled`, before `initializeInner()` runs.

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

## VkmCommandQueueType

```cpp
enum class VkmCommandQueueType : uint8_t { Graphics=0, Compute=1, Transfer=2, Count=3 };
```
`VkmDriverBase::_commandQueues` is indexed by `(uint8_t)queueType`.

## Adding a New Backend

1. Derive `VkmDriver<Name>` from `VkmDriverBase`, override all 6 pure virtuals.
2. Derive `VkmSwapChain<Name>` from `VkmSwapChainBase`, override all 4 pure virtuals.
3. Derive `VkmCommandBuffer<Name>` from `VkmCommandBufferBase`, override all 5 pure virtuals.
4. Derive `VkmPipelineState<Name>` from `VkmPipelineStateBase`, override both pure virtuals (`createInner`/`destroyInner`), and return it from `VkmDriver<Name>::newPipelineStateInner()`.
5. Add CMake flag `VKM_USE_<NAME>_API` and guard source inclusion in `src/vkm/CMakeLists.txt`.
6. Do not modify any existing method signatures in this directory.
7. Call `VkmGpuCrashHandler::recordSubmission()` in the new `VkmCommandQueueBase::submit()`
   override (see "GPU Crash Handler" above), and wire a device-lost/GPU-error detection path
   that calls `reportCrash()`.

## Code Review Checklist for Common Interface Changes

- [ ] No new pure virtual methods without implementing them in ALL existing backends (Vulkan + Metal + WebGPU)
- [ ] `FRAME_BUFFER_COUNT` not changed
- [ ] `VkmResourceHandle` layout changes (e.g. the `generation` field added for view weak-references) must update BOTH `operator==`/`!=` AND the `std::hash` specialization together — they are not auto-derived
- [ ] Public method signatures in base classes unchanged (breaking ABI)
