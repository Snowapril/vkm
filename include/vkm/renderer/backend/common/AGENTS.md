# Common Renderer Backend — Interface Contracts

This directory defines the abstract interfaces every backend must satisfy. Do not add platform-specific code here.

## Pure Virtual Methods — Must Be Overridden in Every Backend

### VkmDriverBase (`driver.h`)
```cpp
virtual bool initializeInner(const VkmEngineLaunchOptions* options) = 0;
virtual void destroyInner() = 0;
virtual VkmTexture* newTextureInner() = 0;
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
- `VkmResourceType`: Texture=0, Buffer=1, StagingBuffer=2.
- Pooled resources: `handle.isPooledResource()` true when `poolType != Undefined`.

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

- [ ] No new pure virtual methods without implementing them in ALL existing backends (Vulkan + Metal)
- [ ] `FRAME_BUFFER_COUNT` not changed
- [ ] `VkmResourceHandle` memory layout unchanged (used in hash specialization)
- [ ] Public method signatures in base classes unchanged (breaking ABI)
