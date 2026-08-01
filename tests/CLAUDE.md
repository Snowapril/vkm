# CLAUDE.md — tests/

## Rule: every test case has a time budget

`UnitTests.cpp` gives every registered test case a default budget (`kDefaultTestTimeoutSeconds`)
and doctest fails any test that runs longer. A test that is legitimately slower declares its own
budget **at the test**, and a declared budget always wins:

```cpp
TEST_CASE("imports a large scene" * doctest::timeout(20.0)) { ... }
```

Do not raise the default to accommodate one slow test — decorate that test instead, so the
exception is visible where it applies.

Three mechanisms, because they cover different failures:

- **doctest's own check** is post-hoc: a test that overruns but *returns* is reported as an
  ordinary failure and the run continues through the remaining tests.
- **A watchdog thread** aborts the process once a test overruns by `kHangTimeoutFactor` (with a
  `kMinHangGraceSeconds` floor), naming the test and its file:line. This is what stops a test
  that never returns from hanging the whole run with no output — the failure mode that once left
  79 test cases unrun.
- **A wall-clock watchdog in `scripts/run_tests.py`** (`--test-timeout`, default 600 s) kills the
  whole binary and reports FAIL. The first two both measure time spent *inside* a test case, and
  are native-only besides, so neither can see a hang in fixture construction, in driver teardown
  after the last test, or inside a signal handler. The last of those is not hypothetical: a VMA
  leak assert at driver teardown aborts inside a handler that allocates, which recurses through
  backward-cpp and presents as a **spinning** process rather than an idle one, so nothing short
  of an external kill ends it.

`TestTimeBudget.cpp` guards both: it asserts no test is left unbudgeted, and it deliberately
overruns a tight budget under `doctest::should_fail()` to prove overruns really do fail.

Current headroom: the slowest test is ~0.09 s on Metal and ~0.33 s on Vulkan, so the default is
roughly 30x the measured worst case. If a slower CI runner trips it, the failure message names
the test — give that one its own budget rather than relaxing the default.

`UnitTests.cpp` passes `argc`/`argv` to doctest, so a single test can be re-run with
`--test-case="<name>"` and durations printed with `--duration=true`.

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

### Pixel readback

`VkmDriverBase::readbackTexture(handle)` is the engine-level CPU readback API — use it for
any pixel-comparison test (see `TestBackbufferReadback.mm` and `TestMetalBindlessTriangle.mm`
for the pattern). The former direct-`[MTLTexture getBytes:]` exception is resolved; new tests
must not introduce direct-API exceptions without a documented reason.

### Rationale

- Backend-specific casts and raw API calls tightly couple tests to one backend, defeating
  the purpose of the cross-backend abstraction layer.
- Synchronisation logic duplicated in tests risks incorrect results (missed flushes, wrong
  memory barriers) and is harder to maintain than a single engine-owned implementation.
- Keeping all GPU work inside the engine ensures tests remain valid when a new backend
  (e.g., WebGPU) is added, without touching any test source file.
