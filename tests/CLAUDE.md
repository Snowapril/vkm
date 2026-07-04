# CLAUDE.md — tests/

## Rule: No direct graphics API calls in unit test code

Unit tests must **never** call graphics-backend APIs (Metal, Vulkan, WebGPU, etc.) directly.
All GPU work — command buffer recording, submission, synchronisation, and readback of pixel
data into a CPU-visible buffer — must be driven exclusively through the vkm engine's own
abstractions (`VkmDriverBase`, `VkmCommandBufferBase`, `VkmCommandQueueBase`, `VkmRenderGraph`,
and any future readback helpers added to the engine).

### Prohibited patterns

```cpp
// WRONG — direct Metal API calls in a test
id<MTLCommandBuffer> cmd = [mtlQueue commandBuffer];
id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
[enc endEncoding];
[cmd commit];
[cmd waitUntilCompleted];
[texture getBytes:pixels.data() ...];   // raw CPU readback
```

```cpp
// WRONG — casting through the abstraction to reach the raw handle
auto* q = static_cast<vkm::VkmCommandQueueMetal*>(driver->getCommandQueue(...));
id<MTLCommandQueue> raw = q->getMTLCommandQueue();  // must not be used in tests
```

```cpp
// WRONG — direct Vulkan calls in a test
VkCommandBuffer vkCmd = ...;
vkCmdBeginRendering(vkCmd, &info);
vkQueueSubmit(vkQueue, 1, &submitInfo, fence);
```

### Acceptable use of backend-specific accessors

Fetching a backend handle and asserting it is non-null/non-zero is **permitted** — that
tests engine initialisation correctness, not GPU behaviour:

```cpp
// OK — validity assertions only, no GPU work driven through the raw handle
CHECK(vkDriver->getDevice() != VK_NULL_HANDLE);
CHECK(vkDriver->getPhysicalDevice() != VK_NULL_HANDLE);
CHECK(metalDriver->getMTLDevice() != nil);
CHECK(metalQueue->getMTLCommandQueue() != nil);
```

The prohibition applies when the raw handle is used to *drive GPU work* (record commands,
submit, synchronise, read back memory) rather than simply verify it is live.

### Required approach for GPU work

Drive all GPU operations through engine-level APIs:

```cpp
// Correct — use the engine's render graph and command abstractions
vkm::VkmRenderGraph* graph = ...;
auto* subGraph = graph->beginGraphicsSubGraph(frameBufferDesc);
graph->compile();
graph->execute();
graph->ensureCompleted();
// Pixel readback must be done via a vkm readback API (to be added to VkmDriverBase /
// VkmCommandBufferBase) that internally handles staging buffers and synchronisation.
```

Tests that need CPU-side pixel data must request it through a dedicated engine function
(e.g., `driver->readbackTexture(handle, ...)`) that the engine team adds to the abstraction
layer. Until that API exists, tests that require pixel comparison should be left as stubs
with a `// TODO: implement when engine readback API is available` comment.

### Current exception — TestBackbufferReadback.mm

`TestBackbufferReadback.mm` currently drives a Metal render pass and performs `[MTLTexture getBytes:]`
readback directly because `VkmDriverBase` has no CPU readback API yet. This is a **temporary
exception**. When `VkmDriverBase::readbackTexture(handle, ...)` (or equivalent) is added to the
engine, this file must be refactored to remove all direct Metal calls. New tests must not
introduce further direct-API exceptions without a documented reason.

### Rationale

- Backend-specific casts and raw API calls tightly couple tests to one backend, defeating
  the purpose of the cross-backend abstraction layer.
- Synchronisation logic duplicated in tests risks incorrect results (missed flushes, wrong
  memory barriers) and is harder to maintain than a single engine-owned implementation.
- Keeping all GPU work inside the engine ensures tests remain valid when a new backend
  (e.g., WebGPU) is added, without touching any test source file.
