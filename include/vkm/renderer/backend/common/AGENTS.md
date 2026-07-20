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
virtual void onSetDebugName(const char* name) = 0;
// Only when built with VKM_ENABLE_GPU_BREAD_CRUMBS (see "GPU Crash Handler" below):
virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) = 0;
virtual void onEndCommandBuffer() = 0;
```

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

## VkmCommandQueueType

```cpp
enum class VkmCommandQueueType : uint8_t { Graphics=0, Compute=1, Transfer=2, Count=3 };
```
`VkmDriverBase::_commandQueues` is indexed by `(uint8_t)queueType`.

## Adding a New Backend

1. Derive `VkmDriver<Name>` from `VkmDriverBase`, override all 6 pure virtuals.
2. Derive `VkmSwapChain<Name>` from `VkmSwapChainBase`, override all 4 pure virtuals.
3. Derive `VkmCommandBuffer<Name>` from `VkmCommandBufferBase`, override all pure virtuals
   (6 unconditional, plus `onWriteCompletionMarker`/`onEndCommandBuffer` under
   `VKM_ENABLE_GPU_BREAD_CRUMBS`).
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
