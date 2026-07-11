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
struct VkmResourceHandle { uint64_t id; VkmResourcePoolType poolType; VkmResourceType type; };
```

- Allocated by `VkmRenderResourcePool` — do not construct raw handles manually.
- `id == (uint64_t)-1` means invalid. Use `handle.isValid()`.
- `VkmResourceType`: Texture=0, Buffer=1, StagingBuffer=2, Sampler=3, TextureView=4, BufferView=5.
- Pooled resources: `handle.isPooledResource()` true when `poolType != Undefined`.

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

## Per-Resource GPU Usage Tracking

`VkmRenderResource` (`render_resource.h`) tracks the last timeline value it was used with, per
queue, in a fixed `std::array<VkmGpuEventTimelineObject, (uint8_t)VkmCommandQueueType::Count>`:
```cpp
void recordUsage(VkmCommandQueueType queueType, VkmGpuEventTimelineObject timelineObject);
VkmGpuEventTimelineObject getLastUsage(VkmCommandQueueType queueType) const;
bool hasAnyPendingUsage() const; // non-blocking poll via queryLastCompletedTimeline()
```
Only the latest usage per queue is kept — an earlier submit on the same queue is implied
complete once a later one is. `VkmRenderSubGraph::addReferencedResource(VkmResourceHandle)`
lets descriptor-binding recording code declare which resources a subgraph touches;
`VkmRenderGraph::execute()` calls `recordUsage()` for each after the subgraph's commands are
submitted, tagged with that submit's `VkmGpuEventTimelineObject`. `execute()` currently only
ever submits to the `Graphics` queue (hardcoded), so all recorded usage is tagged
`VkmCommandQueueType::Graphics` until the render graph dispatches to multiple queue types.

## Deferred Destruction

Never call `VkmRenderResourcePool::releaseResource()` directly on a resource that may have
been GPU-used — it destroys the resource's native handle **immediately and synchronously**,
with no regard for in-flight GPU work. Once a resource has (or may have) recorded usage via
`recordUsage()`, route its destruction through `VkmDriverBase::getDeferredReclaimer()->requestRelease(handle)`
instead. `VkmDeferredResourceReclaimer` (`deferred_resource_reclaimer.h`) snapshots the
resource's per-queue recorded usage, and only calls the real `releaseResource()` once every
one of those timelines has completed (checked via the non-blocking `queryLastCompletedTimeline()`,
never a blocking `waitIdle()` — that would defeat the purpose).

A dedicated background thread (mutex + condition_variable, woken on new requests or a ~4ms
periodic timeout) drives this on Vulkan/Metal/WebGPU-desktop-hypothetical platforms; it is
owned by `VkmDriverBase`, started at the end of `initialize()`, stopped as the first step of
`destroy()` (draining any still-pending entries with a bounded blocking `waitIdle()` — the
one place blocking is acceptable, since this only runs at shutdown). **On WASM**, which
cannot spawn a blocking background thread, `start()` is a no-op; the same non-blocking sweep
is instead driven once per frame via `pollOnce()`, called unconditionally from
`VkmRenderGraph::execute()` right after the frame's submit (a harmless redundant extra check
on the other backends, where the real thread already does the work).

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

## Code Review Checklist for Common Interface Changes

- [ ] No new pure virtual methods without implementing them in ALL existing backends (Vulkan + Metal + WebGPU)
- [ ] `FRAME_BUFFER_COUNT` not changed
- [ ] `VkmResourceHandle` memory layout unchanged (used in hash specialization)
- [ ] Public method signatures in base classes unchanged (breaking ABI)
