# Implementation Notes

Running log of implementation work in this repo. Updated while a plan is being carried out;
see `CLAUDE.md` §11 for the policy.

## 2026-07-17 — Render graph visualization (capture + in-app inspector)

- `VkmRenderSubGraph` gained a string name (optional trailing param on `begin*SubGraph`),
  `getDependentSubGraphIds()`, and `VkmRenderGraphicsSubGraph::getFrameBufferDescriptor()`.
- `VkmCommandBufferBase` records a per-recording `_boundPipelineHistory`; the capture takes
  per-subgraph deltas of it to attribute pipelines to passes.
- New `copyTexture`/`copyTextureToBuffer` on the command buffer (Metal: MTL4 compute-encoder
  texture-copy selectors; Vulkan/WebGPU: error-logging stubs), gated by the new
  `VkmDriverCapabilityFlags::TextureContentCapture` (Metal only).
- `VkmDriverBase::readbackTexture()` implements the API spec'd in TestBackbufferReadback.mm
  (staging + one-off submit + wait + map), and that test's reference-PNG comparison was
  restored per its embedded instructions.
- New `VkmRenderGraphCapture` (common/render_graph_capture.{h,cpp}): Idle/Armed/Pending/Ready
  state machine; hooked into `VkmRenderGraph::execute()` via
  `VkmRenderGraphCommitOptions::capture`. Snapshots color attachments post-commit (skipping
  AllowPresent backbuffers -- framebufferOnly stays YES per user decision) and reads back up
  to 64 KiB of each referenced buffer.
- Engine: F10 (ImGui IO) arms a capture, `--capture-render-graph` arms at startup; the
  capture frame takes one deliberate `ensureCompleted()` hitch before `finalize()`. Snapshot
  handles are re-referenced on the ImGui overlay subgraph each frame while Ready so the
  deferred reclaimer waits for in-flight ImGui draws on release.
- New `VkmRenderGraphInspector` ImGui window (pass list, pipelines, dependencies,
  inputs/outputs tables, `ImGui::Image` snapshot previews via new
  `VkmImGuiRendererBase::getTextureID()` -- Metal override only -- and a hex viewer for
  captured buffers).
- Triangle sample: names its pass and declares its vertex/index buffers via
  `addReferencedResource()` (first real producer of that API).
- New `tests/TestRenderGraphCapture.mm` covers metadata, snapshot pixel contents, and buffer
  readback headlessly.

### 2026-07-17 — Metal fixes surfaced by the first real GPU readback
- Planned: `copyTexture`/`copyTextureToBuffer` were expected to work with plain MTL4
  compute-encoder copy calls, and `readbackTexture` to reuse the existing waitIdle path.
- Did instead: (1) added explicit consumer/producer barriers
  (`barrierAfterQueueStages:beforeStages:` / `barrierAfterStages:beforeQueueStages:`,
  `MTL4VisibilityOptionDevice`) around all three Metal copy ops -- Metal4 does no automatic
  hazard tracking, so the copy encoder read attachment data before the render pass's writes
  completed (readback returned all zeros). The crash-handler completion-marker writes were
  deliberately left barrier-free (they exist to observe partial execution).
  (2) Fixed `VkmGpuEventTimelineMetal::waitIdle` to wait on `_lastAllocatedTimeline` instead
  of `_lastCompletedCachedTimeline` -- the cached value is stale (usually 0), so every wait
  returned immediately without waiting for in-flight work; Vulkan's implementation already
  waits on the last allocated value, so this aligns Metal with the intended semantics.
- Why: both were pre-existing latent gaps that only became observable now that readback
  verifies GPU results on the CPU; conservative fixes limited to the copy ops and the one
  timeline wait, leaving all other recording paths untouched.

## 2026-07-20 — Xcode GPU capture (MTLCaptureManager, Metal 4)

- `--enable-gpu-capture` now creates a frame-aligned `MTLCaptureScope` ("vkm frame") on the
  Graphics MTL4 queue and installs it as `MTLCaptureManager.defaultCaptureScope`; new
  cross-backend `VkmDriverBase::onFrameBegin()/onFrameEnd()` hooks (called from
  `VkmEngine::loopInner()`, no-ops off-Metal) begin/end the scope each frame.
- One-shot `.gputrace` export via `requestGpuFrameCapture()` — F9 hotkey or the new
  `--gpu-capture-frame` flag (implies `--enable-gpu-capture`) — written to the working
  directory. `MTL_CAPTURE_ENABLED=1` is auto-set before Metal device creation by peeking
  at raw process args in `platform/apple/application.mm`.
- Verified end-to-end: triangle `--gpu-capture-frame` wrote a valid 8.4 MB `.gputrace`
  bundle, validation-clean; smoke test added to `TestRenderGraphCapture.mm`.

## 2026-07-21 — Multiple windows (per-window swapchain) + ImGui on a secondary window

- `VkmEngine` no longer holds a single `_mainSwapChain`; it owns a
  `std::vector<VkmWindowContext>`, one per window. Each `VkmWindowContext` carries its
  swapchain, native handle, back-buffer format, an `_isImGuiWindow` flag, and its own
  `FRAME_COUNT` render graphs (the per-frame render graphs moved off the engine into the
  context). `addSwapChain(windowInfo, isImGuiWindow=false)` now returns a window index and
  de-singletonizes (the `_mainSwapChain == nullptr` assert is gone); `getSwapChain(index)` /
  `getWindowCount()` added, `getMainSwapChain()` kept as a window-0 shim.
- `VkmEngine::render()` iterates windows: each window independently throttles+resets its
  own frame-slot graph, acquires its own back buffer, records, then does one
  `execute()`/submit carrying only its own `presentSwapChain`, then presents. This keeps the
  backend invariant "exactly one presentSwapChain per submit per frame" intact with N windows
  (one submit per window, not per frame). `prepareRender()` folded into that loop and removed.
- ImGui is bound to exactly one window (`isImGuiWindow=true`). Its overlay subgraph now
  targets that window's back buffer. A dedicated ImGui window clears its attachment (nothing
  else draws to it); the single-window degrade case (WASM) loads instead, so the sole window
  shows the app scene with the ImGui overlay on top.
- `AppDelegate::render` gained a leading `uint32_t windowIndex`; the engine calls it once per
  non-ImGui window (and, in single-window mode, for the sole window). Both samples updated.
- Platform apps create a second window: GLFW platforms (linux/windows/apple-vulkan) open a
  960x640 "ImGui" GLFW window and `addSwapChain(..., true)`; the loop polls both via a single
  `glfwPollEvents()` and vetoes closing the ImGui window (main-window close quits). Apple/Metal
  opens a second `NSWindow` + `CAMetalLayer` (RGBA16Float) driven by the existing single
  `CAMetalDisplayLink` — its callback pulls `[secondaryLayer nextDrawable]` each frame so both
  swapchains stay on one cadence (no second display link, which would desync the frame ring).
  WASM stays single-window (a browser tab can't host a second OS window) and marks that window
  the ImGui window.
- `VkmSwapChainVulkan::createSwapChain` now asserts
  `vkGetPhysicalDeviceSurfaceSupportKHR(graphicsFamily, surface)` — with two surfaces the
  hard-coded Graphics[0] present queue is verified per surface instead of assumed.

## 2026-07-24 — Build time reduction

Four independent causes of the long build, in order of impact:

- **The vendored dxc build ran single-threaded on the critical path.** The nested
  `ExternalProject_Add(dxc_build ...)` is a separate CMake invocation, so the outer
  `cmake --build --parallel N` never reached it, and with the pinned `Unix Makefiles`
  generator that meant `make -j1` over a full LLVM/Clang fork (~4700 translation units).
  It is not optional work: `UnitTests` → `tests_triangle_shaders` → `vkm-compiler` →
  `dxc_build`. Fixed by passing `--parallel ${VKM_DXC_BUILD_JOBS}` (from
  `ProcessorCount`).
- **That build was also repeated per build tree.** Its output lived in
  `${CMAKE_BINARY_DIR}/dxc-build`, so `build/metal`, `build/vulkan`,
  `build/host-tools`, every git worktree and every `run_clean` paid it again. Moved to
  `dependencies/dxc-macos-build` (shared, out of tree) and the whole ExternalProject is
  now skipped when `VKM_DXC_EXECUTABLE` already exists. That variable is a cache entry,
  so `-DVKM_DXC_EXECUTABLE=<path>` also lets an externally supplied dxc (e.g. the one in
  the Vulkan SDK) replace the source build outright. `run_clean.py --dxc` wipes it.
- **glslang and meshoptimizer were built but unreferenced.** Nothing under `src/`,
  `include/` or `tests/` names `glslang`, `ShaderLang`, `GlslangToSpv`, `TShader`,
  `TProgram`, `EShLang` or `meshopt` — shader compilation goes HLSL → SPIR-V through dxc
  and SPIR-V → MSL through spirv-cross, never through glslang. Together they were ~100
  translation units, and `vkmcore` linked an 83 MB `libglslang.a` (Debug) plus a
  2280-byte, i.e. empty, `libSPIRV.a` for no symbols. Both `add_subdirectory` calls and
  the `vkmcore` link line are gone; the sources stay in `bootstrap.json` so re-enabling
  is a CMake-only change. `vkmcore`'s `spirv-cross-core` link was dead for the same
  reason (`spirv_cross` appears only in `vkm-compiler`) and was dropped too, along with
  the unused spirv-cross C++/reflection/util writers.
- **No compiler cache, and the slowest available generator.** The root CMakeLists now
  enables ccache via `CMAKE_<LANG>_COMPILER_LAUNCHER` when it is installed, and
  `run_tests.py`/`run_sample.py` select the Ninja generator when `ninja` is present, not
  on Windows, and the build directory is fresh. Both are pure auto-detection: with
  neither tool installed the build behaves exactly as before. The generator condition
  matters for more than speed — compiler launchers are ignored by the Xcode generator,
  so Ninja is what makes the ccache detection actually take effect.

`.github/workflows/macos.yml` caches the dxc binary (keyed on the `dxc-macos-src`
revision), the bootstrapped dependency sources, and the ccache store. The cached dxc
entry covers `bin/` *and* `lib/libdxcompiler.dylib` — `bin/dxc` is a symlink to
`dxc-3.7`, which loads the dylib through an `@executable_path/../lib` rpath and cannot
run without it.

Measured on an 11-core macOS host, Release, metal backend, Makefiles generator, no
ccache (`cmake --build --target UnitTests --parallel 11`):

| Scenario | Wall clock |
| --- | --- |
| Cold tree, dxc built from source | 4m27s (267s wall / 1667s CPU) |
| Any later tree, dxc reused | 40s configure + build + run tests |
| `--target triangle` after that | 4.8s incl. shader-cache regeneration |

The 267s figure is the *whole* cold build; dxc is ~230s of it at a 6.2x parallel
speedup. The equivalent single-threaded time is not measured here, but it is bounded
below by dxc's own CPU time (~1520s ≈ 25min), which is what the old `-j1` nested build
spent serially. Cold builds of a second and third backend tree drop from that same ~25
minutes to roughly the 40s row, since dxc is no longer rebuilt per tree.

Verified after the change: 84/84 doctest cases (16919 assertions) pass on the metal
backend with Metal API Validation enabled and no validation diagnostics; the triangle
sample builds its `.vfcache` shaders through the dxc → spirv-cross → metallib chain and
renders without validation errors. The one `ld: warning: ignoring duplicate libraries:
libspirv-cross-core.a` on the vkm-compiler link predates this work — the generated link
line names it twice both before and after — and is left alone.

## 2026-07-24 — glTF 2.0 scene import (Phase 1: geometry) + model_viewer sample

- Chose glTF 2.0 (`.gltf`/`.glb`) as the engine's only runtime import format; OBJ/FBX/USD
  stay offline conversion sources. Parser is **cgltf** v1.15 (`dependencies/bootstrap.json`),
  a single-header C library with no exceptions — emscripten builds compile without
  `-fexceptions`. Its implementation lives alone in `renderer/scene/cgltf_impl.cpp` so
  third-party warnings don't collide with `-Werror`.
- New `renderer/scene/` module: `scene_model.{h,cpp}` (CPU-side `VkmSceneVertex` /
  `VkmSceneMesh` / `VkmSceneMaterial` / `VkmSceneNode` / `VkmSceneModel` with draw-list
  flattening and AABBs), `gltf_importer.{h,cpp}` (cgltf → the fixed 64-byte vertex layout,
  meshoptimizer vertex-cache/fetch optimization, exception-free error reporting), and
  `scene_model_gpu.{h,cpp}` (per-mesh vertex/index buffers registered into the bindless set,
  exactly as the triangle sample does by hand). `meshoptimizer` was already vendored but had
  never been linked into `vkmcore`; it is now.
- New `src/samples/model_viewer/`: orbit camera, depth buffer sized to the swapchain
  (recreated through the deferred reclaimer on resize), per-draw push constants carrying
  MVP + bindless slots + base color + object-space light direction, and per-pixel Lambert
  shading of the material's `baseColorFactor`.
- New `scripts/download_scenes.py` fetches sample scenes (DamagedHelmet, Sponza) from
  KhronosGroup/glTF-Sample-Assets into `resources/Scenes/`, which is now gitignored — assets
  are fetched like dependencies, not committed.
- Tests: `TestGltfImporter.cpp` (fixtures `resources/tests/gltf_triangle.gltf` embedded-base64
  and `.glb`) covers meshes/materials/hierarchy/draw-list/bounds, normal generation, and
  graceful failure on missing/truncated files; `TestSceneModelRender{,Metal}.{cpp,mm}` +
  `TestSceneModelRenderShared.hpp` render an imported mesh offscreen through the
  model_viewer PSO with a depth attachment and assert the material color reached the pixels.

## 2026-07-25 — Scene rework: vertex layouts, geometry pools, GPU-driven groundwork

- **Vertex layouts.** `VkmSceneVertex` (fixed 64 B) is gone. `renderer/scene/vertex_layout.{h,cpp}`
  describes a layout as attributes + stride and provides three presets — `PositionOnly` (16 B),
  `StandardPBR` (64 B, byte-identical to the old struct so the shader ABI is unchanged) and
  `Compact` (32 B, snorm8x4 normal/tangent + f16x2 uv). `vkmRead/WriteVertexAttribute` is the only
  place a storage format is decoded, and packing uses meshoptimizer's own `quantizeHalf` /
  `quantizeSnorm` rather than hand-rolled bit twiddling. `VkmSceneMesh` now holds interleaved bytes
  plus its layout; the glTF importer packs into whichever preset `VkmGltfImportOptions` names and
  skips normal generation for a layout that has nowhere to put one.
- **Geometry pools.** New `VkmSceneGeometryPool`: one mega vertex + index buffer per layout preset,
  so a scene costs two bindless slots per layout instead of two per primitive. Both are published
  as untyped u32 word arrays, which is what lets one pool hold any stride.
- **Scene layer.** New `VkmScene` replaces `VkmSceneModelGpu`: it aggregates imported models as
  placed objects (honouring the node hierarchy, which the old per-mesh path bypassed), sorts them
  by (pipeline, layout, material) into draw batches, and publishes `VkmObjectData` /
  `VkmFrameData` through three new fixed bindless singleton bindings. The device-side ObjectData
  buffer is a single buffer so an object index is frame-invariant; the per-frame ring lives on the
  staging side instead.
- **Draw path.** `model_viewer.hlsl` pushes no constants at all: `SV_InstanceID` is the object
  index (passed as `firstInstance`), and everything else hangs off ObjectData/FrameData. One
  permutation per layout preset comes from the PSO JSON `options`. Shading moved to world space,
  which is what makes `_normalTransform` load-bearing.
- **WebGPU bug fixed.** `VERTEX_ELEMENT_STRIDE = 32` was mis-addressing every scene mesh past the
  first (scene vertices were 64 B). Both mega-buffers are now `array<u32>` with word offsets in the
  slot table, which also brings WebGPU in line with Vulkan/Metal's opaque byte-range treatment.
- **Compute enablement.** Compute-only PSOs can now be authored in JSON (the parser required a
  vertex stage while option expansion rejected vertex+compute, so no compute PSO was reachable);
  `dispatch()`, `drawIndirectCount()` and `barrierIndirectArgumentBuffer()` exist on all three
  backends; push constants work in a compute pass; Metal opens/closes the compute pass on pipeline
  bind/unbind and binds the bindless argument table there; WebGPU binds bind group 0 for compute
  and routes push constants to the compute encoder. `VkmRenderComputeSubGraph` and
  `VkmRenderTransferSubGraph` finally have callbacks, so they can record anything at all.
- **GPU-driven draws.** `VkmScene::recordCull()` records two dispatches into one compute pass:
  `resources/Shaders/scene_cull.hlsl` (shared by every backend) frustum-tests each object's
  world-space bounding sphere and compacts the survivors into a per-batch index list with
  `InterlockedAdd`; `scene_emit_draws.hlsl` turns that into `VkmDrawIndirectArguments`, zeroing the
  slots past the visible count. `recordDrawBatches()` then issues one `drawIndirectCount()` per
  batch instead of one draw per object. Closing and reopening the compute pass between the two
  dispatches is what orders the emit's reads after the cull's writes. A new `VisibleList` bindless
  singleton carries the handoff, and the batch bounds travel as compute push constants.
- These are the first real engine PSOs, which surfaced two pieces of latent build wiring: a
  dependency cycle (`vkmcore` -> `vkm_engine_shaders` -> `vkm-compiler` -> `vkmcore`, fixed by
  having the executables that load the cache depend on it rather than the library) and the fact
  that engine PSO json and engine HLSL live in different directories, which needed a `SHADER_ROOT`
  option on `vkm_add_shader_cache_target`. Under Emscripten `vkmcore`'s `RESOURCES_DIR` now points
  at `/resources/` and each wasm executable preloads the engine PSO and shader-cache directories.
- Testing culling needed care: the frustum planes come from the same view-projection the rasterizer
  clips against, so anything culling rejects would have been clipped anyway and the rendered pixels
  cannot tell the two apart. The test reads the visible count back instead and asserts 1 -> 0 -> 1
  as the object leaves the frustum and returns. That immediately found a real bug -- thread 0 of the
  emit pass returned early when nothing was visible, leaving the argument buffer's count word
  holding the previous frame's value.
- Still to come (see TODO.md): the Metal-only emit variant that fills an
  `MTLIndirectCommandBuffer` from MSL, and WebGPU render-bundle caching.

## 2026-07-24 — CPU/GPU memory statistics + ImGui Memory Inspector

- The engine tracked memory but never showed it, and the two "actual" numbers were missing
  entirely. Added `getProcessMemoryStats()` to `platform/common/process_stats.h` (one
  implementation per OS, next to the existing CPU-usage ones) and
  `VkmDriverBase::getGpuMemoryStats()` with Metal (`currentAllocatedSize`,
  `recommendedMaxWorkingSetSize`, `MTLHeap.usedSize`/`currentAllocatedSize`) and Vulkan
  (`vmaGetHeapBudgets` over device-local heaps, `vmaCalculateStatistics`' block-vs-allocation
  split) overrides. WebGPU keeps the base default of "nothing to report".
- New `renderer/memory_report.{h,cpp}` joins those with `MemoryTracker` and
  `VkmRenderResourcePool` into one `VkmMemorySnapshot`, plus `logMemoryReport()` and
  `formatByteSize()`. Deliberately ImGui-free so the shutdown dump and the unit tests share
  the capture path with the UI.
- New `renderer/imgui/memory_inspector.{h,cpp}` (F8) shows process/CPU/GPU tracked-vs-actual
  sections, a top-32 table of CPU tags and a per-category GPU table; `VkmEngine` samples it at
  2 Hz (the tracker's global mutex is on every allocation's path), the debug overlay reads the
  same cached sample, and `VkmEngine::destroy()` logs a full report before teardown.
- `vkmResourceTypeName()` moved into `renderer_common` because the report needed the same
  mapping the render graph inspector had privately; the inspector now uses the shared one.
## 2026-07-24 — macOS Metal app registers as a foreground UI app

- The samples are plain executables, not `.app` bundles, and the Metal path drives
  `NSApplication` directly, so macOS registered the process as `type="BackgroundOnly"`
  (verified with `lsappinfo`): no Dock tile, no menu bar, no Cmd-Tab entry. The Vulkan path
  gets this for free from GLFW (`dependencies/src/glfw/src/cocoa_init.m:638`).
- `VkmApplication::entryPoint` now sets `NSApplicationActivationPolicyRegular` and installs a
  minimal app menu (Hide / Hide Others / Show All / Quit) before `NSApplicationMain`;
  `applicationDidFinishLaunching` calls `[NSApp activate]` so the window is key on launch.
- Side effect: as a foreground app the `CAMetalDisplayLink` is no longer throttled (5 s run
  went from ~940 to ~2670 log lines), which made the pre-existing shutdown race fire on every
  exit — teardown never stopped the render thread, so it logged into spdlog after its statics
  were gone (`[*** LOG ERROR #0001 ***] ... mutex lock failed`). Fixed below.

## 2026-07-24 — macOS Metal shutdown path stops the render thread

- The render thread was detached and ran `-[NSRunLoop run]`, which has no exit; nothing ever
  invalidated the `CAMetalDisplayLink`. Since `-terminate:` exits the process directly (main()
  never regains control on Metal, so `VkmApplication::destroy` never ran), the display-link
  callback kept driving the engine and the logger through static destruction.
- `RendererCoordinatorController` is now stoppable: the worker publishes its `CFRunLoopRef`
  (handshake via a `dispatch_semaphore_t` so `-stop` cannot race thread startup) and runs
  `CFRunLoopRun()`; the thread is `PTHREAD_CREATE_JOINABLE`. New `-stop` invalidates the display
  link, calls `CFRunLoopStop`, and `pthread_join`s, so an in-flight callback is waited out.
- `applicationWillTerminate:` now calls `-stop`, releases the coordinator (the file is non-ARC;
  the previous `= nil` leaked it and the display link) and calls `_engine->destroy()`, matching
  what the Vulkan path does in `VkmApplication::destroy`.
- Verified with `MTL_DEBUG_LAYER=1` on all three quit paths (`--auto-close`, ESC, Quit menu
  item, window close): no hang, no `mutex lock failed`, no validation errors.

## 2026-07-25 — Backend divergence moved out of user shaders into a common HLSL header

- `triangle.hlsl` and `model_viewer.hlsl` each carried two byte-identical
  `#if defined(VKM_BACKEND_WEBGPU)` blocks (resource declarations + the vertex/index fetch),
  leaking the bindless binding numbers, the WebGPU push-constant emulation and the slot-table
  layout into every sample. New `include/vkm/shaders/vkm_bindless.hlsli` is now the single
  place that branches on the backend; samples use `VKM_PUSH_CONSTANTS`,
  `VKM_BINDLESS_VERTEX_PULLING`, `VKM_LOAD_INDEX`, `VKM_LOAD_VERTEX` and contain no
  `VKM_BACKEND_*` test at all.
- Macros rather than HLSL 2021 templates: the WebGPU mega-buffer and the Vulkan/Metal
  descriptor array are both `StructuredBuffer<VertexType>` declarations, and only a macro can
  emit a *declaration* parameterized by the sample's vertex struct.
- dxc was being invoked with no `-I` at all, so a shared header was previously impossible.
  `vkm-compiler` gained `--include-dir`; `ShaderCompile.cmake` passes
  `include/vkm/shaders` and globs its `*.hlsli` into every shader command's `DEPENDS`.
  That glob is load-bearing — dxc emits no depfile here, so without it editing the header
  would silently not rebuild any `.vfcache`.
- Verified as a no-op at the ABI level rather than by inspection: MSL regenerated for both
  samples diffs against the pre-change baseline only as resource renames plus a moved
  `type_PushConstant_PushConstants` declaration (source order) — `[[id(4096)]]`,
  `[[id(8192)]]`, `[[buffer(2)]]`, `[[buffer(3)]]` unchanged, fragment stages byte-identical.
  For WebGPU (no local runtime, `VKM_COMPILER_ENABLE_WGSL=OFF`), old vs new SPIR-V
  disassembly is identical once `OpName`s are dropped and ids normalized. All 12
  backend × shader × stage dxc combinations compile. Metal suite 97/97 passed, and touching
  the header regenerates all 6 caches.
## 2026-07-25 — macOS leak tooling + per-frame command buffer / Metal object leaks

- New `scripts/detect_leaks.py`: runs a sample under `MallocStackLogging=1` and snapshots it
  with `leaks`/`heap` on an interval, reporting unreferenced leaks separately from
  still-referenced heap growth. Each snapshot saves a `.memgraph`, and the last is compared to
  the first with `leaks --diffFrom=` so only leaks that appeared while running are listed.
  Driven by the `detect-leak` skill. `MTL_DEBUG_LAYER` is deliberately left off — the debug
  layer retains command buffers and descriptors of its own and skews both numbers.
- It found ~4700 leaks per 30 s in `model_viewer` (metal), one set per frame:
  - `VkmCommandEncoderMetal::beginRenderPass` never released its `MTL4RenderPassDescriptor`
    and `VkmCommandQueueMetal::submit` never released its `MTL4CommitOptions` (these sources
    are non-ARC).
  - `VkmCommandBufferPoolBase::release()` had no callers at all, so `allocate()` constructed a
    new command buffer instance — and with it a new `MTL4CommandBuffer` — every frame, even
    though the base class already resets per-use state for reuse. `VkmRenderGraph::execute()`
    and the `uploadToBuffer`/`readbackTexture` one-off submits now return theirs to the pool.
  - `VkmCommandBufferMetal` now owns the +1 reference that
    `getOrCreateRHICommandBuffer()` hands over: it releases the previous use's command buffer
    when the pool hands the slot out again (one frame later at the earliest) rather than
    straight after commit.
- After the fixes the sample's physical footprint is flat (213.2 MB -> 213.0 MB over 40 s,
  was +14.2 MB per 30 s) and 2 new leaks (416 B) appear over a minute, none from vkm code.
- Not fixed, logged in `TODO.md`: Vulkan's `getOrCreateRHICommandBuffer()` has the same shape
  of leak (`vkAllocateCommandBuffers` per acquire, never freed); WebGPU already releases its
  encoder in `submit()`.
## 2026-07-25 — Cubemap loading + skybox sample (first textured render path)

- vkm had never sampled a texture: no image decoder, no upload path, no `registerTexture`, no
  sampler bound anywhere. A cubemap is the first consumer of all four, so most of this change
  is general texture infrastructure and only the last part is skybox-specific.
- `VkmTextureType {Auto, Cube}` on `VkmTextureInfo`/`VkmTextureViewInfo`. `Auto` reproduces the
  old `_numArrayLayers > 1` inference exactly, so no existing call site changes behavior. Cube
  sets `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` + `VK_IMAGE_VIEW_TYPE_CUBE` on Vulkan, and
  `MTLTextureTypeCube` with `arrayLength = 1` on Metal — Metal counts `arrayLength` in whole
  cubes, so passing 6 there would create a 6-cube array. `initializeTextureCommon` asserts the
  6-layers/square invariant once, centrally, rather than letting the two backends diverge.
- `copyBufferToTexture` / `uploadToTexture` mirror `copyTextureToBuffer` / `uploadToBuffer`.
  `copyTextureToBuffer` and `readbackTexture` gained an `arrayLayer` parameter at the same
  time — without it a test can only ever see face 0, and face-order is the most likely bug in
  a cubemap.
- **Latent bug fixed:** `transitionImageLayout` hardcoded `subresourceRange = {aspect,0,1,0,1}`.
  Every texture in the engine had been single-mip single-layer, so this never mattered; on a
  6-layer cube it left layers 1-5 in `UNDEFINED` while `VkmTextureVulkan::_currentLayout` (one
  scalar for the whole image) claimed otherwise. Now `VK_REMAINING_*`, which is identical for
  every pre-existing caller.
- Bindless `registerTexture` takes the **texture**, not a view — the default view already has
  the right dimensionality thanks to `_type`, so no extra view object is needed on either
  backend. Set 0 gained one engine sampler at binding 3 (linear, clamp-to-edge), as a Vulkan
  `pImmutableSamplers` binding and a Metal argument-buffer entry at id 12288.
- `vkm_add_shader_cache_target` now depends on `bindless_resource_manager.h`: the generated MSL
  bakes in the argument-buffer ids, so editing that header must invalidate every `.vfcache`.
  The `vkm-compiler` target dependency already covered the normal path, but not
  `VKM_HOST_VKM_COMPILER` (wasm), where the compiler is an opaque prebuilt file.
- The skybox is a generated fullscreen triangle — no vertex/index buffers at all, which suits
  an engine where nothing ever binds one. The view ray is built in the vertex stage and
  interpolated, because the fragment stage cannot read push constants (vertex-only range).
- Verified: `run_tests.py` passes on all three backends. Metal 102/102 with
  `MTL_DEBUG_LAYER=1`, Vulkan 103/103 with `VK_LAYER_KHRONOS_validation` active, WebGPU PASS
  (stubs still compile). Zero validation errors on either backend. The skybox sample was run
  on Metal and visually confirmed: +Z blue centered, +X red on the left, -X cyan on the right,
  matching the right-handed `lookAtRH` convention.

## 2026-07-25 — Host-copy texture upload on unified-memory devices

- `uploadToTexture` had one implementation: staging buffer, one-off command buffer, submit,
  block. Correct for a discrete GPU; on unified memory it copies the pixels twice through
  memory the CPU can already write and stalls the queue once per call.
- Now two paths behind one entry point. Which one runs is the *destination texture's*
  property, not the caller's: `VkmTexture::isHostWritable()` is set at creation from what the
  backend actually allocated, and `VkmResourceUploadMode` (Auto/ForceStaging/ForceHostCopy)
  only selects among what that made available. Two levels of gating --
  `VkmDriverCapabilityFlags::TextureHostCopy` per device, `isHostWritable()` per texture.
- Metal: `MTLStorageModeShared` + `replaceRegion:`, the same mechanism the ImGui font atlas
  already used. Vulkan: `VK_EXT_host_image_copy`, because an OPTIMAL-tiled image cannot be
  memcpy'd into -- host-visible memory alone buys nothing. On Vulkan the outcome is re-checked
  after allocation with `vmaGetAllocationMemoryProperties`, since adding `HOST_TRANSFER` usage
  can change the image's memory-type requirements: a request is not a guarantee.
- Only plain upload destinations are eligible (`AllowTransferDst`, not an attachment or
  presentable). Render targets stay device-local; they are GPU-written and would only lose
  bandwidth.
- The host path does no queue work at all, so it does not block -- which removes the six
  per-cubemap stalls previously recorded in `TODO.md` on any device that supports it.
- **Debugging note.** A null `vkCopyMemoryToImage` (see the deviation below) surfaced as a
  *hang* with mimalloc "heap corruption" assertions, not a stack trace: backward-cpp's signal
  handler allocates while formatting the trace, so the abort re-entered mimalloc and recursed
  forever. The mimalloc output was entirely a red herring. Bisecting the feature in two steps
  (extension enabled but unused, then images created but never written) localized it in two
  runs where reading the assertion text would have misled indefinitely. Logged in `TODO.md`.
- Verified: Metal 102/102 with `MTL_DEBUG_LAYER=1`, Vulkan 103/103 with
  `VK_LAYER_KHRONOS_validation`, WebGPU PASS. Zero validation errors on either backend. The
  cubemap test uploads all six faces through *both* paths and asserts identical readback, with
  the second pass writing different colors so a silently-no-op host copy cannot pass. Both
  skybox samples log "uploaded by direct host copy", confirming the fast path is the one
  actually taken rather than a silent fallback.

## 2026-07-25 — Per-thread CPU profiler + ImGui flame chart

- Nothing CPU-side existed to profile with: `getProcessCpuUsagePercent()` gives one
  process-wide number, `VkmGpuTimerVulkan` times whole frames on the GPU, and the
  `CHROME_TRACING`/`TASKFLOW_PROFILER` CMake options at `CMakeLists.txt:204-205` are read by
  no source file. New `base/cpu_profiler.{h,cpp}` adds `VKM_PROFILE_SCOPE` /
  `VKM_PROFILE_SCOPE_DYNAMIC` / `VKM_PROFILE_SET_THREAD_NAME` and a `VkmCpuProfiler` immortal
  singleton that collects nested per-thread zones into a 240-frame ring.
- Zones are wall-clock `steady_clock` intervals, not per-thread CPU time: a flame chart is
  read to find where a frame went, and time blocked on a fence or a mutex is exactly what
  needs to be visible there.
- Locking is split so the hot path takes nothing shared: a thread's open-zone stack is
  touched only by that thread, while its closed-zone buffer and name sit behind a small
  per-thread mutex the collector takes once per frame. `ThreadState` objects are never
  destroyed, so a thread exiting mid-capture cannot invalidate a buffer `beginFrame()` is
  about to drain. While not capturing, a scope costs one relaxed atomic load.
- A frame's span is the union of its zones rather than the frame driver's own bracket:
  `VkmDeferredResourceReclaimer` polls on a 4 ms cadence that does not align to frame
  boundaries, and clipping to the driver's bracket would drop its work off the chart.
- Zone names are stored by pointer, so `VKM_PROFILE_SCOPE_DYNAMIC` interns its argument --
  `VkmRenderSubGraph::getName()` is rebuilt every frame by `VkmRenderGraph::reset()` while
  the ring holds the pointer for up to 240 frames.
- New `renderer/imgui/cpu_profiler_inspector.{h,cpp}` (F7, or `--gv_cpu_profile=1`) draws a
  clickable frame-time history strip over a per-thread flame chart with a time ruler,
  wheel-zoom about the cursor, drag-pan and hover tooltips. Capture follows the window's
  visibility, and clicking a history bar pins that frame *and* stops capture -- otherwise the
  ring would keep rolling and drop the frame the user asked to look at.
- Instrumented the whole engine frame path (`loopInner`/`update`/`render`, per-window
  acquire/compile/execute/present, each subgraph's `commit()`, `CommandQueue::submit`) plus
  the reclaimer's release pass; thread names are set where each thread starts, so macOS shows
  `RenderThread` and the other platforms `MainThread`.

## 2026-07-25 — Camera into vkm + descriptor set 1 (per-frame constants)

- New `VkmCamera` + `VkmOrbitCameraController` (`renderer/camera.{h,cpp}`), lifted from
  `model_viewer`'s anonymous-namespace `OrbitCamera`. The controller **registers a listener** on
  `VkmInputHandler` (`addListener`, previously used only by a test) instead of polling it, so it
  needs no per-frame tick: listeners run from `beginFrame()`, which `loopInner()` calls before
  `update()`/`render()`. It tracks left-drag state itself and derives cursor deltas from
  consecutive `CursorMove` positions, dropping the tracked position on button-press because ImGui
  may have owned the mouse in between and left it stale.
- `frame()` takes center+radius rather than a `VkmSceneAABB`, keeping `renderer/camera.h` free of
  a `renderer/scene/` dependency; the sample adapts in a 4-line helper.
- Established the set convention in `common/frame_constants.h`: 0 bindless, 1 per-frame,
  2 per-pass (reserved), 3 per-draw (reserved) — ascending update frequency. `VkmFrameConstants`
  is 272 bytes (view, projection, viewProjection, inverseViewProjection, cameraPositionWorld).
- Set 1 implemented on all three backends via `VkmFrameConstantManager*`, each owning a raw
  native uniform buffer with `FRAME_COUNT` regions (Vulkan: host-visible + persistently mapped,
  one descriptor set per slot; Metal: `StorageModeShared`, bound by GPU address at
  `[[buffer(4)]]`; WebGPU: `Uniform|CopyDst` + `wgpuQueueWriteBuffer`, one bind group per slot).
  `VkmEngine::render()` writes the slot after its `ensureCompleted()` and after acquire.
- Metal: `add_discrete_descriptor_set(1)` in vkm-compiler is what keeps set 1 out of a second
  Tier-2 argument buffer. Verified in the emitted MSL:
  `constant type_ConstantBuffer_VkmFrameConstants& g_VkmFrame [[buffer(4)]]`, struct layout
  matching the C++ one field for field. `maxBufferBindCount` bumped 4 -> 5.
- `model_viewer` push constants now carry the model matrix, not the MVP; the shader composes
  `viewProjection * model`. `TestSceneModelRenderShared.hpp` splits its placement transform
  across set 1 (translate) and the push constants (scale) so neither path is an identity that
  could mask a broken binding — that existing Metal offscreen test is now the GPU-level proof
  that set 1 reaches the shader.
- Verification: Metal 105/105 tests (17065 assertions), Vulkan 106/106 (678), both with
  validation on and zero validation output; `model_viewer` and `triangle` run on both. WebGPU is
  compile-verified (vkmcore + sample under emsdk) plus SPIR-V-verified
  (`OpDecorate %g_VkmFrame DescriptorSet 1 / Binding 0`); emitting WGSL needs a tint build no
  local or CI configuration provides (logged in `TODO.md`).

## 2026-07-25 — Per-test time budgets in UnitTests

- `UnitTests.cpp` now gives every registered test case a default budget
  (`kDefaultTestTimeoutSeconds = 10`) by walking `doctest::detail::getRegisteredTests()` and
  setting `m_timeout` where it is still 0, so doctest's own "exceeded time limit" failure
  applies to all ~107 tests without decorating each one. A test that needs longer declares
  `TEST_CASE("..." * doctest::timeout(seconds))`, which the pass leaves alone. The registry is a
  `std::set` whose ordering is line/name/file/template-id only, so the in-place mutation cannot
  break its invariant.
- doctest's check is post-hoc, so it cannot help with a test that never returns -- exactly the
  failure that had left 79 cases unrun. A `TestHangWatchdog` listener therefore tracks the
  running test's deadline on its own thread and kills the process at
  `max(budget * 3, budget + 5s)`, printing the test name and file:line. The two mechanisms don't
  race: an overrun that returns is a normal failure and the run continues; only a stuck test
  reaches the watchdog.
- Budget chosen from measurement, not guesswork: with `--duration=true` the slowest test is
  0.087 s on Metal (`Backbuffer readback - solid red`) and 0.333 s on Vulkan (`Vulkan clip space`),
  so 10 s is ~30x the observed worst case.
- `main()` now forwards `argc`/`argv` to `applyCommandLine`. It previously took no arguments, so
  `--test-case=` and `--duration=` were silently ignored -- which is also what made it impossible
  to re-run just the test the watchdog names.
- New `tests/TestTimeBudget.cpp` guards the mechanism itself: one case asserts no test is left
  unbudgeted, and one deliberately overruns a 0.05 s budget under `doctest::should_fail()` so the
  enforcement is proven from inside a green suite.
- Verified: Metal 107/107, Vulkan 108/108, wasm/WebGPU via headless Chrome all pass; a temporary
  infinite-loop test was killed by the watchdog in 6 s with exit code 1 and correct attribution.

## 2026-07-28 — Host-writable buffers, buffer map/unmap, and GPU virtual addresses

- `uploadToBuffer` had gained a `VkmResourceUploadMode` parameter that nothing read, because there
  was nothing it *could* read: `VkmBuffer` had no `isHostWritable()`, no `map()`, and every backend
  allocated it GPU-only (VMA `AUTO` with no `HOST_ACCESS_*`; `MTLResourceStorageModePrivate`; no
  WebGPU map usage bits). It now has the same two-path shape `uploadToTexture` has had.
- **Host-writability is a caller opt-in for buffers, not backend policy.** New
  `VkmMemoryAccessHint {DeviceLocal, HostWrite}` on `VkmBufferInfo`; the default leaves all nine
  existing `uploadToBuffer` call sites and every engine buffer in exactly today's memory. Textures
  infer it instead, and the asymmetry is deliberate: inferring it for buffers would have moved the
  scene mega-buffers, the ObjectData buffer and the indirect-args buffer into host-visible memory
  at once, which is a bandwidth decision only the caller can make.
- `HostWrite` forces the committed path on both backends — Vulkan's shared pool block is
  device-local and Metal's heap is `MTLStorageModePrivate`, so a host-writable buffer cannot be
  suballocated from either. Combining it with `ForcePooled` warns rather than failing.
- Vulkan reports `isHostWritable()` from `vmaGetAllocationMemoryProperties` after the fact, the
  same "a request is not a guarantee" check `VkmTextureVulkan` already does. `unmap()` is where
  `vmaFlushAllocation` happens: the mapping itself is permanent (`VMA_ALLOCATION_CREATE_MAPPED_BIT`),
  so a separate `flush()` on `VkmBuffer` would have been API surface for nothing.
- **`bufferDeviceAddress` turned out to need no enabling.** `VkmDriverVulkan` already requests every
  feature `vkGetPhysicalDeviceFeatures2` reports and passes that chain straight to `vkCreateDevice`,
  so the feature was on all along and unused. What was missing was
  `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` and `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
  on every buffer — including the pool block, or its sub-allocations could not report an address
  either. A pooled buffer's address is the block's plus `getBufferOffset()`.
- New `VkmDriverCapabilityFlags::BufferDeviceAddress` keeps that optional: a driver without the
  feature adds neither the usage bit nor the allocator flag and reports 0, so nothing about its
  initialization changes. Metal sets the flag unconditionally (`MTLBuffer.gpuAddress`), WebGPU never.
- WebGPU gets no host-write path at all, and this is forced rather than deferred: a `WGPUBuffer`'s
  usage flags fix its map mode for life and `MapWrite` only combines with `CopySrc`, so a buffer
  anything else touches can never be CPU-write-mapped. It warns and stays device-local.
- `VkmStagingBuffer` needed only `getGPUVirtualAddress()` — its `map()`/`unmap()` already existed
  on all three backends and were left untouched.
- New `tests/TestBufferHostWrite{,Metal}.{cpp,mm}` + shared header. The load-bearing assertion is
  that `ForceStaging` and `ForceHostCopy` produce identical bytes while writing *different*
  patterns, so a host write that silently did nothing fails instead of passing on the staging
  pass's leftovers — the same trick that made the cubemap test meaningful. The GPU-address test
  covers committed, pooled and host-writable buffers and asserts their addresses are distinct,
  which is what would catch a pooled buffer reporting the block base instead of its own range.
- Verified: Metal 147/147 (17932 assertions) with Metal API Validation on, Vulkan 148/148 with
  `VK_LAYER_KHRONOS_validation`, WebGPU PASS. Zero validation output on either backend — which
  matters more than usual here, since the new usage bit and allocator flag touch *every* Vulkan
  buffer allocation. On this machine's MoltenVK, `bufferDeviceAddress` is supported and the tests
  observed real, distinct addresses rather than the 0 fallback.

## 2026-07-28 — WASD fly camera + per-window input focus

- **New `VkmFlyCameraController`** next to the orbit one (`renderer/camera.{h,cpp}`): WASD moves
  along the camera basis, Q/E along **world** up so rising stays vertical while pitched, Shift
  boosts, left-drag looks (matching the orbit controller and the skybox `LookCamera`, which were
  the only existing conventions). The orbit controller is untouched, so `model_viewer`'s turntable
  feel is unchanged; a checkbox in its Scene Browser hands the shared camera between the two, with
  `syncFromCamera()` on the way in so the switch is invisible.
- It is the first controller with a `tick(deltaTime)`. Continuous movement cannot be event-driven:
  held keys are polled from `VkmInputHandler::isKeyDown` rather than integrated from key events,
  because auto-repeat rate is an OS setting and would otherwise decide how fast the camera flies.
  The direction is normalized so diagonals are not faster than the axes.
- Switching *back* to orbit reframes on the scene bounds. The orbit controller only touches the
  camera when it receives an event, so without that the view would sit where the fly camera left it
  and then jump on the first drag.
- **The macOS multi-window mouse bug** was `VkmImGuiRendererMetal::newFrameInner` polling
  `[NSApp keyWindow]` unconditionally. At startup that is the *scene* window, so the scene cursor
  drove ImGui's virtual cursor in the ImGui window's `io.DisplaySize` space; wherever it crossed a
  panel's rectangle `WantCaptureMouse` flipped true and `VkmWindowImpl::forwardCursorMoveEvent`
  dropped the event — the camera stalled depending on where in the scene window the cursor was.
  `[NSEvent pressedMouseButtons]` being system-global made a scene drag register as an ImGui click
  on top of that.
- Fixed by giving the engine real focus tracking rather than by patching the poll in place:
  `VkmInputHandler::onWindowFocusChanged` / `getFocusedWindowIndex()`, fed by
  `windowDidBecomeKey:`/`windowDidResignKey:` on macOS and a new `glfwSetWindowFocusCallback` on the
  GLFW backends, with `VkmEngine::findWindowIndex()` mapping a native handle back to an index (the
  first real use of `VkmWindowContext::_windowHandle`, stored since the multi-window work and never
  read). `VkmImGuiRendererBase::newFrame(windowFocused)` carries the answer to the backend, and the
  Metal poll is gated on it; when it is not focused ImGui is told the cursor is outside *and* all
  three buttons are up.
- Resolving the window is deliberately done against the `NSWindow`s the app created, not
  `contentView.layer`: neither view is marked layer-hosting, so AppKit may substitute a layer there
  and the comparison would silently never match.
- **Latent bug the same mechanism fixes:** hold a key, click the other window, and the release goes
  to whoever took focus — `_keyDown` stayed latched forever. Harmless while nothing read held keys;
  it stops being harmless the moment WASD lands. Focus loss now clears held keys and buttons,
  publishing them as release *edges* so a consumer watching the transition still sees one, and drops
  the tracked cursor so the next move does not produce a delta spanning two windows. Both camera
  controllers also end any drag in progress on focus loss.
- A stale focus loss cannot clear a focus another window just took: macOS delivers the gain before
  the previous window's loss, so only the window that still holds focus may clear it.
- Verified: Metal 157/157 (17977 assertions) with Metal API Validation, Vulkan 158/158 with
  `VK_LAYER_KHRONOS_validation`, WebGPU PASS, zero validation output; `model_viewer` and `triangle`
  run validation-clean on Metal. The new headless cases cover the movement basis, dt proportionality
  (two half-ticks == one full tick), boost, normalized diagonals, look-only-while-dragging, pitch
  clamping, `syncFromCamera` round-trip, unregister, and the focus semantics including the
  held-key release. **The two-window interaction itself is not machine-verifiable** — it needs a
  human clicking between the windows — so that part rests on the code path plus the unit-level
  focus tests.

## 2026-07-28 — TODO.md sweep: eight small self-contained fixes

Picked the slice of `TODO.md` that was small, self-contained, and verifiable on this machine.
Two entries turned out to describe less than was actually wrong.

- `VkmRenderResourcePool::_driver` was not merely unstored, it was **uninitialized** — the member
  was declared, the constructor took the pointer, and nothing ever assigned it, so it held an
  indeterminate value for the object's lifetime. Initialized it, added a `protected getDriver()`,
  and dropped `VkmRenderResourcePoolMetal::_driverMetal` in favour of it. Deleting the parameter
  instead was rejected: it would modify a public method in `common/` (the one absolute rule in
  `AGENTS.md`), touch four call sites, and still leave the Metal duplicate.
- `VkmCommandBufferBase::setDebugName()` gained its first caller. The render graph records every
  subgraph into **one** command buffer, so per-subgraph naming is structurally unavailable; the
  frame slot is what the `"<queueName>#<index>"` fallback actually lacks, and subgraph identity
  is already carried by the debug group and the completion markers.
- Metal's depth attachment now honours `VkmDepthStencilAttachmentDescriptor`. The converters were
  already there and already applied to color twelve lines above. Also widened the guard to require
  **both** the framebuffer and render-pass depth optionals — they are set independently, and
  reading the descriptor on the strength of the handle alone is UB. Metal was the only backend
  not checking both.
- Metal residency: membership is now tracked explicitly instead of inferred from "does this handle
  have an allocation". `onResourceInitialized` skips resources whose native object does not exist
  yet (the swapchain backbuffer, whose drawable arrives later via `overrideExternalHandle`), but
  `releaseResource` removed anything that had one by then. `registerExternalAllocation` /
  `unregisterExternalAllocation` became idempotent as a side effect.
- Metal raster state (fill/cull/winding) is applied at bind time. It is *encoder* state on Metal,
  not part of `MTLRenderPipelineState`, and nothing ever set it — `setTriangleFillMode:`,
  `setCullMode:` and `setFrontFacingWinding:` appeared nowhere in the tree. Second-order effect
  worth knowing: a PSO omitting `rasterization_state` now gets the descriptor's default
  `cullMode = Back` on Metal where it previously got `MTLCullModeNone`. Audited every PSO JSON;
  only `model_viewer` declares back-face culling.
- Vulkan now frees command buffers. Not eagerly the way Metal does — see the deviation below.
- `CHROME_TRACING` went from dead to functional: `VkmCpuProfiler::exportChromeTrace()` writes
  Chrome Trace Event Format built on the profiler's existing frame/zone model. `TASKFLOW_PROFILER`
  was deleted rather than repaired; its stray comma made the variable literally
  `TASKFLOW_PROFILER,` so its guard tested a never-defined name, and there is no taskflow code
  anywhere to profile.
- New `tests/TestRasterState{.cpp,Metal.mm,Shared.hpp}` renders the wireframe PSO variant offscreen
  and asserts the triangle interior is empty. Proven as a real repro, not just a passing test:
  with the fill mode forced back to `Fill` the interior pixel reads (94, 102, 60) and the test
  fails. Runs on both Metal and Vulkan.
- New `TestCpuProfiler` case asserts the exporter's microsecond units and containment nesting.
- Verified: Metal, Vulkan **and** wasm/WebGPU all pass (147 cases on Metal). The Vulkan free path
  was measured on the triangle sample at 22000 acquires with the pending list never exceeding 0
  and no validation output. Also configured once with `-DCHROME_TRACING=OFF` to confirm the build
  stays green with the export compiled out.
- Deliberately untested, and why: no public API exposes the live `VkCommandBuffer` count and
  `MTLResidencySet` has no membership query, so the Vulkan and Metal residency fixes are proven by
  validation-layer silence plus a sample soak rather than by assertions. Adding accessors purely
  for tests would be the speculative API `CLAUDE.md` §2 forbids.
- Surfaced, not fixed (`CLAUDE.md` §3): `~VkmCommandBufferPoolBase` leaks every pooled
  `VkmCommandBufferBase*`; `VkmRenderResource::initializeCommon` validates `_handle` *before*
  assigning it, so its guard can never fire; the taskflow include paths in `CMakeLists.txt:265`
  and `tests/CMakeLists.txt:116` are now fully orphaned.

## 2026-07-30 — Per-subgraph GPU profiler + per-queue timeline viewer

- The CPU side could answer "where did the frame go"; the GPU side could not. `VkmGpuTimerVulkan`
  timed **whole frames**, on Vulkan only, and reached the debug overlay through a
  `static_cast<VkmDriverVulkan*>` in `engine.cpp` — Metal and WebGPU showed "GPU: n/a". New
  `renderer/backend/common/gpu_profiler.{h,cpp}` (`VkmGpuProfiler`) times each render graph
  subgraph and groups zones by the command queue that ran them; `renderer/imgui/
  gpu_profiler_inspector.{h,cpp}` (F6, or `--gv_gpu_profile=1`) draws them in milliseconds.
  `vulkan_gpu_timer.{h,cpp}` is deleted and the overlay's stat is now backend-free.
- Same collector/front-end split as the CPU profiler, and the same interaction model on purpose:
  history strip, pin-a-frame, ruler drag-select, wheel-zoom about the cursor. `zoneColor()` and
  `chooseGridStepMs()` moved out of the CPU inspector's anonymous namespace into
  `imgui/profiler_chart_common.h` so a subgraph gets the **same** color in both charts — which is
  what makes reading them side by side worth anything.
- Recording is always on where the device supports timestamps; only the frame *history ring* is
  gated on `isCapturing()`. That is what keeps the always-visible overlay stat alive with the
  window closed, and it is the one place this deliberately differs from the CPU profiler.
- Slots are a ring of fixed buckets, not a frame-indexed ring. `VkmGpuTimerVulkan` keyed its pool
  by frame slot (`2 * FRAME_COUNT` queries), which `implementation-notes.md` already noted breaks
  with multiple windows submitting per frame. Buckets are handed out and retired in submission
  order, so `collect()` can stop at the first one the GPU has not finished, and it never blocks:
  it resolves only submissions whose `VkmGpuEventTimelineObject` has already completed.
- `beginSubmission()` takes an exact zone count and resets exactly `2 * zoneCount` slots. Reserving
  more would be worse than wasteful on Vulkan: a slot that is reset but never written stays
  permanently unavailable to `vkGetQueryPoolResults` (`VUID-vkGetQueryPoolResults-None-09401`).
- Metal writes timestamps at **command-buffer** scope
  (`[MTL4CommandBuffer writeTimestampIntoHeap:atIndex:]`), like `pushDebugGroup` already does, not
  through the per-encoder `writeTimestampWithGranularity:` variants. That is what lets a zone wrap
  a whole subgraph without splitting an encoder, and `VkmCommandEncoderMetal` is untouched. Ticks
  are scaled by `1e9 / [MTLDevice queryTimestampFrequency]` rather than assumed to be nanoseconds.
- WebGPU forced the shape of the seam. It has no encoder-level timestamp write at all — a
  begin/end pair can only ride one pass descriptor's
  `beginningOfPassWriteIndex`/`endOfPassWriteIndex`, which must be filled *before* the pass opens.
  Hence `beginGpuZone(beginSlot, endSlot)` receives both slots up front on every backend, and
  `endGpuZone()` returns a bool: a zone that enclosed no pass (a transfer subgraph, or the
  submission-wide zone around the subgraphs) is never written there, and the profiler drops it
  rather than report a span it never measured. Consequently `getLastFrameGpuTimeMs()` latches the
  submission's whole *span* rather than specifically its depth-0 zone.
- `vkmWriteGpuChromeTrace()` is a free function that `exportChromeTrace()` forwards its ring to,
  so the format is testable from hand-built frames — the profiler cannot produce one without a
  device. It uses pid 2 (the CPU export uses 1) so both JSONs load into one viewer without their
  rows colliding. The two are still on different clocks and cannot be overlaid; logged in `TODO.md`.
- Tests: `TestGpuProfiler.cpp` covers range aggregation and the trace format with no device;
  `TestGpuProfilerCapture.mm` drives a real two-subgraph render graph through `VkmRenderGraph` only
  (per `tests/CLAUDE.md`) and asserts a `MainGraphics` timeline whose depth-0 zone contains both
  depth-1 subgraph zones with real elapsed time.

## 2026-07-25 — Render graph capture: PSO hot reload, input previews, texture browser

Three additions to the render graph capture tooling, all behind the (now toggleable, F5)
`VkmRenderGraphInspector` window, which became a three-tab window: **Capture** / **Pipelines** /
**Textures**. It used to render nothing at all unless a capture was `Ready`.

### PSO hot reload
- **In-place recreate is the whole design.** Every holder of a PSO outside
  `VkmPipelineStateManager` keeps a raw non-owning `VkmPipelineStateBase*` — sample members, the
  per-frame render-callback lambdas that capture it by value, and `VkmCommandBufferBase`'s bound-
  pipeline history — with no invalidation hook anywhere. New
  `VkmPipelineStateBase::reload(desc, shaderCacheDir, outError)` calls `destroyInner()` then
  `createInner(newDesc)` on the *same object*, so no cached pointer ever changes. That makes the
  whole class of "who still points at the old pipeline?" questions vacuous, and it is why the
  alternative (swapping the `unique_ptr` in the manager's map) was rejected outright.
- On failure it re-`destroyInner()`s and rebuilds the previous descriptor, so a bad edit leaves a
  working pipeline rather than a destroyed one. Caveat, deliberately not engineered around: if the
  shader compiled but the new render state did not, the rollback pairs the *new* shaders with the
  *old* state. Still a valid pipeline, which is the contract.
- Backend pipeline objects are destroyed synchronously (`vkDestroyPipeline`; PSOs never go through
  the deferred reclaimer), so new `VkmDriverBase::waitIdle()` drains every command queue once per
  reload batch. Reload runs from `VkmEngine::update()`, before `render()` records anything.
- `VkmFormat::Swapchain` resolution moved out of `newPipelineState()` into
  `VkmDriverBase::resolveSwapChainFormats()`, because `reload()` bypasses the former. Missing this
  would have silently broken every reloaded PSO that uses the sentinel — which is every sample's.
- **Reload is per json, not per variant**: `expandPipelineStateOptions` re-runs over the edited
  file and the variant set itself can change. New `VkmPipelineStateSource` records jsonPath /
  shaderCacheDir / origin / variantNames / watched files per loaded json.
- Nothing is destroyed until recompile, parse and expand have all succeeded, so a broken edit is a
  no-op rather than a half-applied reload.

### Runtime shader recompilation
- `vkm-compiler` is invoked as a subprocess with byte-identical arguments to the build-time
  `ShaderCompile.cmake` invocation — including *omitting* `--shader-root`, which the tool defaults
  to the PSO json's own directory. Producer and consumer therefore cannot drift.
- `src/tools/vkm-compiler/subprocess.{h,cpp}` moved to `include/vkm/base/subprocess.h` +
  `src/vkm/base/subprocess.cpp`; vkm-compiler already links vkmcore, so it just includes it now.
  Gained an `__EMSCRIPTEN__` branch returning a failure result (no subprocesses on wasm).
- `VKM_COMPILER_EXECUTABLE` is baked in from the **top-level** CMakeLists (the vkm-compiler target
  is added *after* `src/vkm`, so `$<TARGET_FILE:>` is not resolvable there); the
  `VKM_HOST_VKM_COMPILER` branch stays in `src/vkm/CMakeLists.txt`. When neither exists (installed
  or Emscripten builds) `isShaderRecompilationAvailable()` is false, the checkbox is disabled, and
  reload still re-applies json render state.
- Staleness is a throttled 0.5 s poll of `last_write_time` over the json, its shader sources and
  the shared `*.hlsli` — driven from `update()` next to the memory inspector's sampling, at the
  same cadence. No watcher thread: it would have to synchronize with a path that calls
  `waitIdle()` and destroys GPU objects.

### Input-texture previews
- The color-attachment snapshot body became `VkmRenderGraphCapture::takeTextureSnapshot()`, now
  used by referenced *input* textures too. Net lines went down.
- `snapshotTexture` moved from `VkmCapturedAttachment` into `VkmCapturedResourceInfo` so
  attachments, inputs and (later) the browser share one preview path.
- **No `AllowTransferSrc` requirement on the source.** A sampled input is typically
  `AllowShaderRead|AllowTransferDst`; demanding the flag would have excluded exactly the textures
  this feature exists for. Metal, the only backend that reaches this code, imposes no usage
  restriction on a blit source, and the existing color-attachment path never checked it either.
- Cube/array sources snapshot slice 0 (`copyTexture` is defined as mip 0 / layer 0). **Verified,
  not assumed**: `TestRenderGraphCapture.mm` uploads six differently-colored faces and asserts the
  snapshot holds face 0's color, with `MTL_DEBUG_LAYER=1` clean — a cube-to-2D blit is legal.
- Depth/stencil stays metadata only. Note `vkmBytesPerTexel()` returns 4 for `D32_SFLOAT`, so the
  gate is `hasDepth() || hasStencil()`, not a zero byte size — the first draft got this wrong and
  the new depth test caught it.
- Where no snapshot exists the inspector falls back to previewing the *live* texture, labelled as
  such, so non-Metal backends and cube inputs still show something truthful.

### Texture browser
- New `VkmRenderResourcePool::getAllResourceHandles(type)`, the companion to `getAllMemoryTags()`
  (which reports sizes but no handles). It iterates `_resources` rather than `_memoryTags`, so
  untagged externally-owned resources are not silently dropped, and returns **handles, not
  pointers**: a slot recycled between enumeration and use is rejected by the generation check
  instead of dangling.
- Names come from `VkmResourceMemoryTag::name` (a durable `std::string`), never from
  `VkmTextureInfo::_debugName`, which is a borrowed `const char*`.
- Two preview paths. A plain single-layer 2D texture is shown live and zero-copy via
  `getTextureID()` — the Metal ImGui renderer binds whatever `ImTextureID` a widget emits, so
  there is no per-texture setup. A cube/array texture goes through
  `readbackTexture(handle, layer)` → `uploadToTexture` into one reusable preview texture, only on
  selection/layer change or an explicit Refresh, because that readback blocks on a full queue
  wait. The preview keeps the **source's** format, since `readbackTexture` returns native channel
  order (an RGBA8 preview would swap channels for a BGRA source).
- Textures the inspector emitted an `ImGui::Image()` for are referenced on the ImGui overlay
  subgraph each frame, exactly as the capture's snapshots already were, so their GPU usage is
  tracked while those draws are in flight.
- `formatToString` was file-local in the inspector; it became `vkmFormatName()` in
  `renderer_common.h` next to `vkmResourceTypeName()` rather than being duplicated.

### Two additions beyond the literal request
- **Snapshot byte budget** (`kMaxSnapshotBytes`, 256 MiB): now that inputs are snapshotted too, a
  graph with several passes each referencing a few 4K textures would allocate hundreds of MB on one
  F10 press. ~6 lines, mirroring the existing `kMaxCapturedBufferBytes`.
- **Name filter** in the texture browser: with hundreds of live textures the list is unusable
  without one, and it is also how a developer excludes the capture's own `GraphCapture.*` snapshots
  (which are deliberately listed — hiding them would make a browser that claims "all textures" lie).

### Verified
`scripts/run_tests.py` PASS on all three backends. Metal 106/106 with `MTL_DEBUG_LAYER=1`, Vulkan
109/109 with `VK_LAYER_KHRONOS_validation`, WebGPU PASS. Zero validation errors. The triangle and
skybox samples run clean under `MTL_DEBUG_LAYER=1` with the new tabbed window. The recompile test
really spawns `vkm-compiler` over the triangle sample's HLSL and asserts both the success path and
that a syntax error fails the reload with the compiler's own output while the old pipeline keeps
rendering. Reload/recompile tests were each confirmed to actually execute (not silently skip) by
temporarily inverting an assertion and observing the failure.

**Rebase note (2026-08-01).** Landing this after the descriptor-set-2 work surfaced one new
interaction: `VkmPerPassResourceTableBase` allocates its descriptor set from a layout the pipeline
owns and `destroyInner()` destroys, so a reload that changes a PSO's set-2 declaration leaves any
table already built from it stale. The table's `_pipelineState` pointer itself stays valid (that is
what in-place recreate buys), and only tests build tables today, so nothing holds one across a
reload yet. Logged in `TODO.md` rather than fixed here: the fix is an invalidation/notification path
from reload to tables, which is exactly the observer machinery this design set out to avoid, and
there is no caller to justify it yet. The toggle also moved from F7 to **F5**, since F7 became the
CPU profiler.

**Not verified by a running app**: clicking Reload in the UI while a sample is on screen, and the
cube face selector's visual output. Both need interactive input, which this environment has no way
to drive; every layer beneath them is covered by the headless tests above.

## 2026-08-01 — Probe GI 4.2c: per-frame probe budget and propagation latency

Turns `restir.md` §8 item 4.2c into running code: the probe passes existed but nothing drove
them, so every one of them was reachable only from `tests/`.

- New `VkmProbeVolumeUpdater` (`renderer/probe_volume_updater.{h,cpp}`) appends the whole refresh
  to a `VkmRenderGraph`: scene update -> cull -> capture (every budgeted probe's six faces in one
  render pass, viewport per tile) -> `barrierTextureForShaderRead` -> irradiance blend -> distance
  blend. Round-robin cursor, clamped per round so every probe is refreshed exactly once.
- `VkmProbeVolume` lost its second atlas copy, `advanceFrame()` and `getPrev*Texture()`;
  hysteresis is now `SrcAlpha`/`OneMinusSrcAlpha` blending against the atlas itself. It also now
  releases through the deferred reclaimer rather than the pool directly, since a per-frame
  consumer makes destroy-while-in-flight reachable.
- `VkmProbeCaptureConstants`/`VkmProbeBlendConstants` became probe-independent (see the Deviations
  entry below); the per-probe remainder is pushed as `VkmProbeCapturePushConstants` (16 B) and
  `VkmProbeBlendPushConstants` (8 B), from the vertex stage, forwarded to the pixel stage as flat
  interpolants because Vulkan declares the push-constant range for vertex/compute only.
- `probe_blend.json` gained blend state and lost the previous-atlas binding.
- New `tests/TestProbeVolumeUpdaterShared.hpp`: a driverless convergence-model test, a
  round-robin schedule test (Metal + Vulkan), and a GPU propagation measurement (Metal).

**The measurement, which is the deliverable:** the decay is geometric at `hysteresis` per refresh,
one refresh per `ceil(probeCount / budget)` frames. Measured on a 4-probe volume and asserted
against that model, then projected: the shipping defaults (2048 probes, budget 32, h = 0.97) need
**4864 frames — ~81 s at 60 Hz — to shed 90%** of a light change. Recorded in `restir.md` §12 and
`TODO.md`.

**Verification.** Metal 193/193 with validation clean; Vulkan clean; Release built. Both new tests
were checked to actually fail when sabotaged: `dst_color_blend_factor: zero` breaks the decay
assertions (7 failures), and wrapping instead of clamping the round-robin breaks the coverage
assertion (2 failures). The first sabotage run also exposed a weakness in the test itself —
doctest's `Approx` folds a scale of 1.0 into its epsilon, so absolute comparisons against the
small radiances an 8x8 probe records passed no matter what the atlas held; the checks are now
ratios against the lit value.

The wasm build also had to be fixed: `TEST_ENGINE_PIPELINE_DIR` is defined for the native backends
only, so everything in the new shared header that loads a pipeline is now behind
`#if defined(TEST_ENGINE_PIPELINE_DIR)`. The convergence-model test is arithmetic and stays outside
it, so it runs on every backend.

**Left unverified:** WebGPU cannot compile shaders at all (no Tint/Dawn build), so the rewritten
`probe_capture.hlsl` and `probe_blend.hlsl` are unverified there — including the blend pass's new
push constant, which on that backend is a set-0 uniform buffer rather than a real push constant,
i.e. a genuinely different code path.

## Deviations

Log entries here when an edge case forces a deviation from an agreed plan. Format:

```
### <date> — <short title>
- Planned: <what the plan said>
- Did instead: <the conservative option taken>
- Why: <the edge case that forced it>
```

### 2026-07-30 — VkmDriverMetal's destructor releases the timestamp counter heap
- Planned: the timestamp pool is created in `initializeGpuTimestampPool()` and released in
  `destroyGpuTimestampPool()`, which `VkmDriverBase::destroy()` calls -- the same lifetime every
  other driver-owned object has.
- Did instead: `~VkmDriverMetal()` also calls `destroyGpuTimestampPool()`, making it the one thing
  that destructor does.
- Why: several Metal unit-test fixtures only `delete driver` without calling `destroy()`
  (`TestBackbufferReadback.mm`, `TestMetalDriver.mm`, `TestRenderGraphCapture.mm`), which was
  harmless while nothing they leaked was scarce. A leaked `MTL4CounterHeap` is not harmless: once
  roughly thirty accumulate in one process, `newCounterHeapWithDescriptor:` **segfaults inside the
  AGX driver** rather than returning nil -- which is how the whole suite began failing at
  `TestSceneModelRenderMetal.mm`, a test that passes in isolation. The conservative option was to
  make the resource I added clean up after itself in its owner's destructor, rather than rework
  four existing fixtures' teardown and risk tripping other end-of-life assertions. Releasing twice
  is safe: `destroyGpuTimestampPool()` nils the handle.

### 2026-07-30 — `--gv_gpu_profile=1` only sets visibility, it does not start capture
- Planned: mirror `gv_cpu_profile`'s startup block exactly -- `setVisible(true)` plus an explicit
  `setCapturing(true)`.
- Did instead: `setVisible(gv_gpu_profile.get())` and nothing else.
- Why: `initializeEngine()` runs before `_driver->initialize()`, so there is no `VkmGpuProfiler` to
  start there and `_driver->getGpuProfiler()` segfaulted. The CPU profiler's equivalent works only
  because it is a process-wide singleton with no driver behind it. No replacement call is needed:
  `update()`'s visibility-edge check already sees the window open on the first frame and starts
  capture then. Caught by running the sample, not by any test -- the inspector has no coverage.

### 2026-07-28 — Metal's front-face mapping is 1:1, not inverted
- Planned: mirror `vulkan_pipeline_state.cpp`'s `toVkFrontFace` and map
  `VkmFrontFace::CounterClockwise` to `MTLWindingClockwise`, on the theory that Metal's top-left
  framebuffer origin reverses screen-space orientation the same way Vulkan's does.
- Did instead: mapped 1:1 — `CounterClockwise` to `MTLWindingCounterClockwise`.
- Why: the premise was backwards. `toVkFrontFace`'s own comment says the inversion exists *on
  Vulkan* to cancel `-fvk-invert-y`, "so a PSO declaring `counter_clockwise` culls the same faces
  on Vulkan as it does on **Metal/WebGPU**", and `src/samples/triangle/main.cpp:70-73` states
  +Y-up is "the engine convention, matching HLSL/D3D, Metal and WebGPU; the Vulkan backend flips
  its viewport to match". Vulkan is the single compensating backend. `webgpu_pipeline_state.cpp`
  already maps 1:1 and is correct — it was briefly flagged as inconsistent and is not. Confirmed
  empirically: `TestSceneModelRenderMetal` renders with `cull_mode: back` +
  `front_face: counter_clockwise` and passes under the 1:1 mapping; the inverted mapping would
  have culled its geometry and turned the target black.

### 2026-07-28 — Vulkan command buffers are recycled through a retire list, not freed in setRHICommandBuffer
- Planned: mirror Metal, which releases the previous handle directly in `setRHICommandBuffer`.
- Did instead: hand the outgoing handle to a completion-gated retire list on the pool, swept by
  the next `getOrCreateRHICommandBuffer()`. Following PR review, a completed entry is **reused**
  rather than freed — it moves to an available list and is handed straight back out, since
  `vkBeginCommandBuffer` implicitly resets a buffer in the executable state when the pool carries
  `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` (it already did). Steady-state rendering
  therefore allocates nothing at all, where the free-and-reallocate version churned one command
  buffer per acquire. The pool converges on the peak number of simultaneously-live buffers.
- Why: the two are not equivalent. An `MTLCommandBuffer` is refcounted and the queue retains it
  while executing, but `vkFreeCommandBuffers` on a pending command buffer is
  `VUID-vkFreeCommandBuffers-pCommandBuffers-00047`. `VkmRenderGraph` releases a command buffer
  back to the pool in the same frame it submits it, and `ensureCompleted()` waits only on that
  frame slot, so an eager free would hit a still-pending buffer within the first `FRAME_COUNT`
  frames. The list is polled with `vkGetSemaphoreCounterValue` rather than waited on, so it costs
  nothing on the critical path, and it is bounded by frames in flight.

### 2026-07-28 — exportChromeTrace uses the public frame accessors, not the internal mutex
- Planned: take `ProfilerState::_frameMutex` directly and walk `state._frames`.
- Did instead: built on the public `getFrameCount()` / `copyFrame()`, each of which takes and
  releases that mutex on its own.
- Why: `copyFrame` already deep-copies a frame under the lock, so reaching into the internals
  bought nothing and would have held the lock across the whole event-array build. `beginFrame()`
  runs on the frame-driver thread and must never wait on an export. Same reason the file write
  stays outside any lock.

### 2026-07-28 — render graph names its command buffer with std::to_string, not fmt::format
- Planned: `fmt::format("RenderGraph.Frame{}", _frameIndex)`.
- Did instead: `"RenderGraph.Frame" + std::to_string(_frameIndex)`.
- Why: `fmt` is not directly included in that translation unit, and this avoids adding a header
  for a single integer append. `CLAUDE.md` §7 prefers the STL where it already suffices.

### 2026-07-25 — the test hang watchdog uses _Exit, not abort
- Planned: `std::abort()` once a test overruns its budget by the grace factor.
- Did instead: `std::_Exit(EXIT_FAILURE)` after flushing stdout/stderr by hand.
- Why: `abort()` runs backward-cpp's SIGABRT handler, which tried to symbolize a stack trace from
  the watchdog thread while the hung thread still held the malloc lock. Observed deadlocking
  there — the message printed and then the process sat forever, i.e. exactly the wedged run the
  watchdog exists to prevent. `_Exit` skips handlers and atexit entirely, and everything worth
  reporting is already flushed.

### 2026-07-25 — set 1 on Metal is a discrete binding, not a second argument buffer
- Planned: pin set 1's Tier-2 argument buffer at Metal buffer index 4 and have the runtime keep
  a per-frame-slot 8-byte argument buffer holding the constant buffer's `gpuAddress`.
- Did instead: `compiler.add_discrete_descriptor_set(kVkmFrameConstantSetIndex)`, so spirv-cross
  emits the constant buffer directly as `constant VkmFrameConstants&` at `[[buffer(4)]]`.
- Why: with `pad_argument_buffer_resources` on, spirv-cross's padding walk *throws* unless every
  argument-buffer resource has a registered basetype, and the argument-buffer variant would have
  cost FRAME_COUNT extra MTLBuffers plus their residency entries for one pointer each. Declaring
  the set discrete sidesteps the padding walk entirely and makes the runtime a single
  `setAddress:` — the same shape push constants already use at `[[buffer(3)]]`.

### 2026-07-25 — frame-constant stride derived from the struct instead of hardcoded
- Planned: `kVkmFrameConstantStride = 256`.
- Did instead: `kVkmFrameConstantAlignment = 256` plus a stride computed as `sizeof` rounded up
  to it (512 for the current 272-byte struct).
- Why: the planned struct is 272 bytes, so a literal 256 stride would have overlapped the next
  frame slot — caught by the `static_assert`. Deriving it means adding a member can never
  silently reintroduce the overlap.

### 2026-07-25 — pre-existing resource-pool out-of-bounds fixed to unblock the GPU tests
- Planned: nothing; this file was not in scope.
- Did instead: added the bounds guard that `VkmRenderResourcePool::releaseResource()` already
  had to its three siblings — `tagResource()`, `getResourceMemoryTag()` and the templated
  `getResource<>()` in `render_resource_pool.hpp`.
- Why: `VKM_INVALID_RESOURCE_HANDLE` carries `poolType`/`type == Undefined`, which *equals*
  `Count`, so passing one indexed `_subPools`/`_resources` one past the end. On clean `main` that
  aborted `UnitTests` in `TestEngineSetup.cpp:265` and left 79 test cases unrun — including
  `TestSceneModelRenderMetal`, the only GPU-level check of set 1. Verified pre-existing by
  stashing this branch's work and reproducing the identical abort. Reusing the existing guard
  verbatim was the conservative option; the whole suite now passes (105/105).
### 2026-07-25 — CPU profiler UI pins by frame number and pauses, and clear() resets numbering
- Planned: pin the clicked history bar by its ring index, and keep `copyFrames()` as the one
  UI read.
- Did instead: pin by frame number and stop capture on pin; split the UI read into
  `copyFrameSummaries()` (strip) plus `copyFrame(index)` (one frame); reset frame numbering
  in `clear()`.
- Why: the ring keeps rolling while capture is on, so a pinned ring index silently slides
  onto a different frame and eventually falls off the end -- freezing the ring is the only
  way a pinned frame stays the one the user clicked. Deep-copying all 240 frames every UI
  frame just to draw a bar strip was ~MBs per frame of pure waste. Numbering was reset in
  `clear()` so each capture's first frame reads as `#0` rather than continuing a discarded
  capture's count.

### 2026-07-25 — flame chart defers its initial zoom fit until a non-empty frame
- Planned: fit zoom/pan to the displayed frame's span on first draw.
- Did instead: keep the fit pending until a frame with a non-zero duration arrives.
- Why: `beginFrame()` closes a frame nothing was recorded into yet, so a capture's first
  frame always has zero duration; fitting to it left the chart at ~884000 px/ms with every
  nested zone scrolled off-screen (observed in the triangle sample before the fix).
### 2026-07-25 — geometry pool indices stay mesh-local instead of being rebased
- Planned: `VkmSceneGeometryPool::appendMesh` would rebase each mesh's indices onto the pool's
  own vertex numbering.
- Did instead: indices are appended unchanged, and `MeshRange::_vertexWordOffset` (copied into
  `VkmObjectData::_vertexWordOffset`) supplies the base in-shader.
- Why: the agreed shader ABI computes `base = obj.vertexWordOffset + index * VERTEX_STRIDE_WORDS`,
  so rebasing on the CPU would add the pool base twice. Keeping indices mesh-local is also the
  conservative option for overflow: index values stay bounded by their own mesh's vertex count
  rather than growing with the pool.

### 2026-07-24 — CI caches only the dxc binary, not its build tree
- Planned: `actions/cache` on `dependencies/dxc-macos-build`.
- Did instead: cached `dependencies/dxc-macos-build/bin` only.
- Why: the full LLVM build tree passes 160 MB before it is even half done and ends up in
  the gigabytes, which would dominate the repository's shared 10 GB cache budget. Since
  the configure short-circuits on `EXISTS ${VKM_DXC_EXECUTABLE}`, restoring the binary
  alone is sufficient to skip the whole ExternalProject — caching the object files buys
  nothing.

### 2026-07-24 — emsdk excluded from the CI dependency-source cache
- Planned: cache `dependencies/src` keyed on a hash of `bootstrap.json`.
- Did instead: same, but with `!dependencies/src/emsdk` excluded.
- Why: emsdk alone is ~1.7 GB and would crowd out the far more valuable dxc entry.
  Dropping its directory while keeping its `.bootstrap.json` state entry is safe:
  bootstrap.py's cached-state check also requires `os.path.exists(lib_dir)`, so it
  re-clones emsdk rather than assuming it is present.

### 2026-07-24 — Ninja detection duplicated rather than shared between the two scripts
- Planned: "a shared helper" appending `-G Ninja`, used by `run_tests.py` and
  `run_sample.py`.
- Did instead: an identical `generator_args()` defined separately in each script.
- Why: `run_sample.py` already carries the comment "copied from run_tests.py to keep the
  two scripts independent" over its own duplicated helpers. Introducing a shared module
  would have reversed a deliberate existing decision, which is well outside the scope of
  a build-time change.

### 2026-07-25 — dropped the cross-metric memory peak assertion (CI flake)
- Planned: assert `peak >= resident` in the process-memory test whenever a peak is reported.
- Did instead: removed that check; the test only asserts the resident figure is real and
  covers what the tracker accounts for.
- Why: peak and current come from different OS counters (Linux `ru_maxrss` vs
  `/proc/self/statm`; macOS two ledger fields) sampled non-atomically, so the ordering is
  not guaranteed at any instant. It passed on the Linux WebGPU runner but failed on the
  Linux Vulkan runner in the same CI run — a flake, not a real regression.

### 2026-07-25 — the offscreen scene-model render test is Metal-only
- Planned: a shared render+readback test driven by both a Metal and a Vulkan fixture,
  mirroring TestClipSpaceOrientationShared.
- Did instead: only the Metal fixture drives it; the Vulkan fixture file was removed and its
  shader-cache target/paths scoped to `VKM_USE_METAL_API`. The two-phase reupload-and-render
  was also dropped (slot recycling is already covered headlessly).
- Why: on the CI Vulkan software rasterizer (lavapipe) the test rendered black and then
  SIGSEGV'd in its second render pass — a pattern (two freshly-constructed render graphs on
  one frame slot) that no shipping code uses; the engine and samples reuse per-frame graphs
  via reset(). With no Vulkan ICD on this machine I cannot verify a fix, and real-pixel GPU
  output is only meaningfully checked on the backend I can run, exactly as
  TestMetalBindlessTriangle is Metal-only. The Vulkan depth-attachment path it exercised
  ships and is compile-verified; it is validated through the model_viewer sample on Vulkan
  hardware instead.

### 2026-07-24 — macOS peak memory comes from the footprint ledger, not getrusage
- Planned: report the process peak from `getrusage`'s `ru_maxrss` alongside `phys_footprint`
  as the current figure.
- Did instead: read `task_vm_info`'s `ledger_phys_footprint_peak` (guarded by the returned
  `TASK_VM_INFO_REV3_COUNT`), leaving the peak at 0 on kernels too old to fill it.
- Why: the two are different metrics. A unit test caught the peak (61 MiB from `ru_maxrss`)
  coming out *below* the current footprint (228 MiB) — `ru_maxrss` counts peak resident pages
  and excludes compressed and IOKit-mapped memory, which `phys_footprint` includes. Mixing
  them would have shown a peak lower than the live value in the UI.

### 2026-07-24 — depth attachments had to be implemented in the Vulkan/WebGPU backends
- Planned: Phase 1 (geometry import + model_viewer) needed no backend changes; the sample
  would simply "add a depth attachment".
- Did instead: implemented `_depthStencilAttachment` handling in
  `VkmCommandBufferVulkan::onBeginRenderPass` (depth `VkRenderingAttachmentInfo` + a layout
  transition, with `transitionImageLayout` gaining an aspect-mask parameter) and in
  `VkmCommandBufferWebGPU::onBeginRenderPass` (`WGPURenderPassDepthStencilAttachment`, per
  aspect so a depth-only format doesn't get stencil ops).
- Why: only the Metal backend read the descriptor's depth attachment; the other two ignored
  it silently, so a depth-tested sample would have rendered without depth on Vulkan/WebGPU.
  Both backends' *pipeline* sides already handled depth formats, so the gap was just the
  render-pass begin — small enough that filling it beat shipping a model viewer whose
  geometry self-occludes incorrectly. Verified on Metal (offscreen test with a depth
  attachment bound, validation layer on); the Vulkan path is covered by the same test but
  this machine has no Vulkan ICD, so it is CI-verified only.

### 2026-07-24 — push-constant range raised from 8 to 128 bytes
- Planned: keep the per-draw push constants at `{vertexSlot, indexSlot}` and only extend
  "if needed".
- Did instead: `kVkmBindlessPushConstantSize` is now 128 bytes and the Vulkan pipeline
  layout uses that constant instead of a hard-coded `sizeof(uint32_t) * 2`.
- Why: a model viewer needs a per-draw transform, and 8 bytes cannot hold one. 128 is the
  push-constant size Vulkan guarantees everywhere and fits the Metal/WebGPU rings' existing
  256-byte entry stride, so no other backend constant had to move. Draws still push only
  what their own shader struct needs (the triangle sample still pushes 8 bytes).

### 2026-07-24 — model path is a global variable, not a cxxopts flag
- Planned: `--model <path>` parsed with cxxopts in the sample.
- Did instead: `--gv_model_path=<path>` via `VKM_GLOBAL_VARIABLE`.
- Why: `VkmEngine` owns argv parsing and forwards unmatched tokens to
  `GlobalVariableManager`; a sample has no hook to register extra cxxopts options, and the
  cvar mechanism is the engine's existing answer for exactly this.

### 2026-07-24 — imported tangents are not generated when absent
- Planned: generate tangents (a `meshopt_generateTangents`-equivalent) for assets that omit
  them, alongside generated normals.
- Did instead: `TANGENT` is copied when present and left zeroed otherwise; only normals are
  generated (area-weighted per-vertex, not the spec's flat normals, which would require
  splitting every shared vertex).
- Why: meshoptimizer has no tangent generator, so this would mean hand-rolling a
  MikkTSpace-style pass for data nothing consumes until normal mapping arrives in Phase 2.

### 2026-07-21 — render-graph capture bound to the scene window, not the ImGui window
- Planned: the plan noted capture could bind to the ImGui window's execute (since the
  inspector UI lives there), accepting that scene passes would not be captured.
- Did instead: capture binds to window 0 (the scene window). The ImGui window still
  references the capture's snapshot textures cross-window for `ImGui::Image` previews.
- Why: the render-graph inspector exists to inspect the app's scene passes (the triangle
  pass), so capturing the scene window's graph is what makes it useful; snapshot textures
  are engine-global (owned by the capture, not a swapchain), so the ImGui pass on the other
  window can still sample them for display. GPU-timer timestamps are left in every window's
  execute — the timer rings its own slot with a reset-before-write per call, so N executes
  per frame stay validation-clean (only the reported GPU time becomes per-window-arbitrary).

### 2026-07-20 — .gputrace captureObject: device instead of MTLCaptureScope
- Planned: `MTLCaptureDescriptor.captureObject = _captureScope` for the programmatic
  `.gputrace` export.
- Did instead: `captureObject = _mtlDevice`. The scope remains the `defaultCaptureScope`
  for Xcode's capture button; only the programmatic descriptor uses the device.
- Why: with `MTL_CAPTURE_ENABLED=1`, the GPUToolsCapture layer throws
  `-[MTLCaptureScope traceStream]: unrecognized selector` (uncaught NSException, app
  terminates) when `startCaptureWithDescriptor:` receives a scope created via
  `newCaptureScopeWithMTL4CommandQueue:` — a tooling incompatibility with the new MTL4
  scope API. Device capture is still exactly one frame because start/stopCapture bracket
  a single onFrameBegin()/onFrameEnd() pair.
- Planned: the bindless work was expected to pass existing CI as-is.
- Did instead: two follow-up fixes after the first PR run. (1) The triangle sample's
  Emscripten `LINK_DEPENDS` on `triangle_shaders.stamps/renderpass.0.stamp` moved under
  the `if (TARGET triangle_shaders)` guard — the wasm CI configures without
  `VKM_HOST_VKM_COMPILER`, so no rule produces the stamp and make failed with "No rule
  to make target". (2) The dxc ExternalProject now passes
  `-Wno-unknown-warning-option -Wno-invalid-specialization`: the macOS 26 CI runner's
  AppleClang rejects DXC's vendored `llvm/ADT/StringRef.h` specializing
  `std::is_nothrow_constructible` as a default-error, and this PR is the first to make
  CI build dxc at all (UnitTests now depends on vkm-compiler via tests_triangle_shaders).
- Why: both are the minimal fixes that keep behavior identical where the builds already
  worked; third-party DXC sources are not patched, the diagnostic is disabled for that
  nested build only.

### 2026-07-15 — unplanned fixes required to get the WebGPU triangle running
- Planned: the bindless plan assumed the WebGPU backend's existing swapchain/render-pass
  scaffold worked and only draw/copy/push-constant/bind-group code was missing.
- Did instead: three additional fixes, all exposed the first time the sample actually
  rendered/presented on WebGPU: (1) `VkmSwapChainWebGPU::presentInner` no longer calls
  `wgpuSurfacePresent` (emdawnwebgpu aborts under the requestAnimationFrame main loop;
  the browser presents implicitly); (2) `onBeginRenderPass` sets
  `depthSlice = WGPU_DEPTH_SLICE_UNDEFINED` (zero-init means "3D slice 0", rejected for
  2D attachments); (3) the triangle sample links with `ALLOW_MEMORY_GROWTH=0` +
  fixed 128 MiB heap, the same documented V8 TextDecoder-vs-resizable-ArrayBuffer
  workaround tests/CMakeLists.txt already uses. Also fixed the never-exercised
  `VKM_TINT_EXECUTABLE` path in the root CMakeLists (tint_cmd outputs to the Dawn build
  root, not bin/).
- Why: pre-existing latent bugs in the never-run WebGPU present/render path blocked the
  plan's "webgpu triangle renders in Chrome" verification; each fix is the minimal
  established-pattern option.

### 2026-07-15 — wasm.yml shader-cache wiring deferred
- Planned: Phase 4 optionally wires the host vkm-compiler + tint build into wasm.yml
  with a `.webgpu.vfcache` artifact check.
- Did instead: deferred as a TODO.md line; CI stays build-only without caches.
- Why: the tint/dawn ExternalProject adds a 15+ minute native build per CI OS and needs
  an actions cache to be tolerable — the plan explicitly allowed deferring on cost.

### 2026-07-15 — Metal bindless triangle test has no pixel comparison
  **[Resolved 2026-07-16: `VkmDriverBase::readbackTexture()` now exists and both
  TestBackbufferReadback.mm and TestMetalBindlessTriangle.mm assert real pixels --
  with direct pixel-value comparisons rather than the PNG-reference machinery the
  original stub sketched.]**
- Planned: the bindless plan's Metal test would "read back center/corner pixels
  following TestBackbufferReadback.mm's existing readback".
- Did instead: TestMetalBindlessTriangle.mm drives the full bindless draw path
  (register/upload/bind/push/draw) headlessly under the Metal validation layer, with
  pixel comparison left as a stub TODO, exactly like TestBackbufferReadback.mm.
- Why: TestBackbufferReadback.mm no longer performs raw readback -- it was stubbed
  pending an engine `readbackTexture()` API, and tests/CLAUDE.md requires pixel tests
  to stay stubs until that API exists. Adding a readback engine API mid-plan (new
  command-buffer hooks on all three backends) would have been larger scope than the
  bindless task itself.

### 2026-07-15 — MSL argument-buffer layout pinned by explicit ids, not padding
  **[SUPERSEDED — this was the bug behind the invisible Metal triangle; see the
  2026-07-16 correction below]**
- Planned: set spirv-cross's `pad_argument_buffer_resources` so the set-0 argument
  buffer struct layout stays fixed across shaders.
- Did instead: dropped `pad_argument_buffer_resources`; assumed the explicit `[[id(N)]]`
  remaps alone determine Tier-2 entry offsets (id*8). The MSL inspection only verified
  the [[id]] attributes were present, not the members' byte offsets.
- Why: with padding enabled, spirv-cross requires a `basetype` on every resource
  binding including the special argument-buffer/push-constant pin entries, which have
  none, and rejects them ("Unexpected argument buffer resource base type").

### 2026-07-16 — ImGui invisible on Metal: resources were never resident
- Planned: n/a (user-reported: "why imgui not rendered?" after the triangle fix).
- Root cause: the Metal ImGui renderer allocates its vertex/index/uniform MTLBuffers
  and font textures raw (bypassing the resource pool) and binds them by
  gpuAddress/gpuResourceID -- but MTL4 residency is explicit and only pool-created
  resources joined the queue's residency set, so the GPU read the ImGui geometry as
  zeros (no fault, no validation error; the path had never been exercised before Metal
  draws worked). Fixed by registering every ImGui allocation via the pool's
  registerExternalAllocation and a new unregisterExternalAllocation mirror (residency
  sets retain members, so grow-reallocated buffers must be removed explicitly).

### 2026-07-16 — argument-buffer padding re-enabled (invisible-triangle fix)
- Planned (debug plan): `[[id(N)]]` is only an argument-index attribute; the Metal
  compiler lays the struct out sequentially, so without padding the vertex-buffer array
  sat at byte 0 while the runtime wrote at byte 32768 -- the shader read null pointers
  and only the clear color rendered.
- Did: re-enabled `pad_argument_buffer_resources` and gave the two pin entries
  `SPIRType::UInt` basetypes (the set-0 lookup entry the argument-buffer pin inserts at
  index 2 is inert: the padding walk jumps from the texture binding at id 0 with count
  4096 straight to 4096). Generated MSL now emits `_m0_pad [[id(0)]]` (4096 textures),
  making byte offsets equal id*8 as the runtime assumes.
- Also fixed while verifying: `VkmGpuEventTimelineMetal::waitIdle` waited on the cached
  *completed* timeline value (an immediate no-op) instead of the last *allocated* value
  like Vulkan does -- GPU-ordering hid it until CPU readback needed a real wait.

### 2026-07-14 — per-image swapchain storage sized past FRAME_BUFFER_COUNT
- Planned: the reviewed swapchain design sized per-image arrays (render-finished
  semaphores, back-buffer handles) to `FRAME_BUFFER_COUNT`, relying on the existing
  `minImageCountClamped == imageCount` assert.
- Did instead: added `MAX_BACK_BUFFER_COUNT` (8) for per-image storage, relaxed the
  assert to a range check, filled `_backBuffers` with `VKM_INVALID_RESOURCE_HANDLE`
  (members were previously indeterminate when default-initialized), and made
  `releaseResource()` reject invalid/out-of-range handles before any indexing.
- Why: the first lavapipe CI run proved Mesa's X11 WSI legally creates more swapchain
  images than requested, firing the assert on all Ubuntu vulkan jobs; the enlargement
  then exposed the garbage-handle release path (heap corruption via out-of-bounds
  `_subPools` indexing). `FRAME_BUFFER_COUNT` itself stays 3 per the AGENTS.md rule.

### 2026-07-13 — VKM_NEW_TAGGED: no escape hatch added
- Planned: harden the macro against non-literal labels, keeping `allocate()`/`trackedNew()`
  as an escape hatch for legitimate non-literal static-duration call sites.
- Did instead: fixed the single existing non-literal call site (a test passing a
  `constexpr const char*` for DRY) to pass the literal directly; no escape hatch added.
- Why: the only non-literal use was incidental, so an unused escape-hatch mechanism
  would have been speculative surface area.

### 2026-07-13 — origin-isolation test rewritten for collision-error policy
- Planned: add a test for the new Engine/User pipeline-name-collision error.
- Did instead: also rewrote the pre-existing "Engine and User origins are isolated"
  test to use two distinct names plus cross-origin lookup assertions.
- Why: that test loaded the same name into both origins and asserted both succeeded,
  which directly contradicts the newly agreed collision-is-an-error policy.

### 2026-07-13 — WebGPU compute pass tied to pipeline bind/unbind
- Planned: mirror Metal's beginComputePass lifecycle for the WebGPU compute pass encoder.
- Did instead: begin the compute pass lazily in `onBindPipeline` and end it in
  `onUnbindPipeline`.
- Why: the common command-buffer interface exposes no begin/end-compute hooks and
  Metal's own `beginComputePass` is never invoked anywhere (dead scaffolding); adding
  new common-interface entry points would have been invasive plumbing beyond scope.

### 2026-07-13 — MTL4 depth/stencil check stayed documentation-only
- Planned: encoder-time validation that the render pass depth/stencil format matches
  the PSO's declared format.
- Did instead: tightened the explanatory NOTE in `metal_pipeline_state.mm` only.
- Why: `beginRenderPass` runs before any pipeline is bound and the encoder does not
  retain the chosen format, so the comparison is unreachable without new plumbing —
  the plan's explicit conservative fallback.

### 2026-07-13 — macOS Vulkan sample: runtime loader fix instead of link fix
- Planned: capture and fix a linker error in the Vulkan sample build on macOS.
- Did instead: the sample already built and linked cleanly; fixed the actual failure —
  `glfwVulkanSupported()` returning false at runtime — by calling `volkInitialize()` +
  `glfwInitVulkanLoader(vkGetInstanceProcAddr)` before the check (with volk headers
  reordered ahead of glfw3.h so the declaration is visible).
- Why: the TODO line described a stale symptom; modern macOS `dlopen` no longer
  searches `/usr/local/lib`, so GLFW could not find the Vulkan loader that volk finds
  via its absolute-path fallback.

### 2026-07-13 — swapchain semaphore debug names held in locals
- Planned: debug-name the new semaphores following existing patterns (string passed
  directly).
- Did instead: store each generated name in a `const std::string` local before
  assigning `pObjectName`.
- Why: inline `fmt::format(...).c_str()` produced a dangling temporary rejected by
  `-Werror,-Wdangling-gsl`.

### 2026-07-12 — session-report marker rewound to df1d9bb
- Planned: after publishing the first session report, write current `HEAD` into
  `.claude/.session-report-marker`.
- Did instead: wrote `df1d9bb` (the merge commit) rather than `HEAD`.
- Why: `HEAD` moved to `f15daac` (bindless vertex-pulling draw path) mid-session from a
  parallel session/worktree. That commit was not covered by the published report, so
  marking `HEAD` would have made the next `/session-report` silently skip it. Rewinding
  the marker to the last reported state is the conservative option: worst case the next
  report re-describes a commit, never drops one.

### 2026-07-25 — The skybox needs no `-fvk-invert-y` compensation
- Planned: negate the `ndc.y` used for ray reconstruction under `#if defined(VKM_BACKEND_VULKAN)`,
  on the theory that Vulkan's vertex-stage `-fvk-invert-y` would flip the emitted position but
  not the separately-computed direction, mirroring the sky vertically on Vulkan only.
- Did instead: no compensation at all, in shared HLSL.
- Why: the premise was wrong. `-fvk-invert-y` is a store-time transform on the `Position`
  builtin — it rewrites the value on the way out, after the shader body has run — so the
  vertex and its interpolants move together and both backends land the same direction on the
  same pixel. Adding the guarded negation would have *introduced* the mirroring it was meant
  to prevent. This is now pinned by `runSkyboxRenderTest`, which renders offscreen and asserts
  +Y is at the top and -Y at the bottom on Metal and Vulkan alike. (The real trap is nearby
  and is documented in `skybox.hlsl`: reconstructing from `SV_Position` in the *pixel* shader
  would be backend-dependent, because the fragment `SV_Position` is post-flip.)

### 2026-07-25 — The engine sampler is created natively, not via `newSampler()`
- Planned: the bindless manager creates its default sampler through `VkmDriverBase::newSampler()`.
- Did instead: `vkCreateSampler` / `newSamplerStateWithDescriptor` directly, with the handle
  owned by the bindless manager.
- Why: `VkmDriverBase::initialize()` calls `initializeInner()` — which is where each backend
  initializes its bindless manager — *before* `_renderResourcePool->initialize()`. A pooled
  resource cannot be created at that point (on Metal the residency sets do not exist yet).
  Creating the native object sidesteps the ordering entirely, and as a bonus avoids setting
  `supportArgumentBuffers = YES` on every sampler the engine ever creates just to satisfy this
  one; `metal_sampler.mm` is left untouched.

### 2026-07-25 — Test FOV is deliberately wider than the sample's
- Planned: the offscreen skybox test mirrors the sample camera, 60-degree FOV.
- Did instead: 120 degrees in the test only.
- Why: a cube face spans +/-45 degrees about its axis, so a 60-degree FOV on a square target
  never leaves the +Z face and every pixel comes back blue — the test would assert nothing
  about the other five faces. The first run failed exactly this way before the FOV was widened.

### 2026-07-25 — Vulkan host image copy must use the EXT entry points
- Planned: call `vkCopyMemoryToImage` / `vkTransitionImageLayoutEXT`.
- Did instead: `vkCopyMemoryToImageEXT` for both, plus a null-pointer check on the exact
  entry points before advertising `VkmDriverCapabilityFlags::TextureHostCopy`.
- Why: `vkCopyMemoryToImage` is the Vulkan 1.4 *core* name, and volk only loads it on a 1.4+
  device. MoltenVK here reports 1.3.334 and exposes the feature through
  `VK_EXT_host_image_copy`, so the core pointer was null and the call crashed. The check had
  to move after `volkLoadDevice` -- placed with the other feature checks it runs before the
  device exists, when every pointer is still null and the capability would never enable.

### 2026-07-25 — Both host-image-copy layout arrays are allocated, not just the one used
- Planned: query `VkPhysicalDeviceHostImageCopyProperties` twice, filling only `pCopyDstLayouts`.
- Did instead: size and point both `pCopySrcLayouts` and `pCopyDstLayouts` at real storage.
- Why: leaving the src pointer null while its count stays non-zero from the first call is a
  half-filled enumerate struct, which drivers handle inconsistently. The src list is unused
  here; allocating it is cheaper than depending on that behavior. (This was investigated as a
  corruption suspect and cleared -- the real fault was the null entry point above -- but the
  defensive form was kept.)

### 2026-07-25 — `writeRegion` is virtual with a default, not pure
- Planned: a pure virtual on `VkmTexture`, overridden per backend.
- Did instead: virtual with a base implementation that logs and returns false.
- Why: a pure virtual made the existing `MockTexture` in `TestEngineSetup.cpp` abstract, and
  more importantly a backend that never reports `isHostWritable()` has nothing meaningful to
  implement. The default is the matching unreachable-case guard, and it let the WebGPU
  override be dropped entirely rather than duplicated.

### 2026-07-25 — Host image copy disabled on MoltenVK after a CI hang
- Planned: enable `VK_EXT_host_image_copy` on any device advertising it with the feature bit set.
- Did instead: additionally require `driverID != VK_DRIVER_ID_MOLTENVK`.
- Why: the `macOS 26 + AppleClang (vulkan)` job hung for 67 minutes with **zero** output after
  linking `UnitTests` (it takes ~2.5 min and runs 98/98 on `main`). stdout is block-buffered
  through the runner's pipe and the run emits ~372 KB locally, so several flushes would have
  appeared had it gotten far — the hang is very early, i.e. in Vulkan driver initialization,
  which is exactly where the new code runs. It did not reproduce locally: the suite passes
  against both the LunarG ICD (1.3.334) and brew's MoltenVK 1.4.1 (1.4.334, the version CI
  installs), so the trigger is something about the runner's loader/layer combination that
  cannot be replicated here.
  The conservative option, and the one taken: MoltenVK emulates this extension on top of Metal,
  and on macOS the engine's own Metal backend already provides the identical optimization
  natively (verified, and its CI job passes). Vulkan-on-Metal therefore loses no real
  capability by falling back to the staging path, which is always correct — whereas leaving it
  on costs a 6-hour CI timeout per run with no diagnostics. Native Vulkan drivers keep the fast
  path. Logged in `TODO.md`, including the resulting coverage gap.

### 2026-08-01 — Probe constants are shared, not per-probe (ring of tables dropped)
- Planned: a ring of `budget` immutable per-pass resource tables, each with its own small uniform
  buffer, because a fragment shader cannot read push constants and set-2 tables are immutable.
- Did instead: **one** shared constant buffer per PSO, plus per-probe values pushed from the
  vertex stage and forwarded as flat interpolants.
- Why: the constants are not actually per-probe. `vkmBuildProbeFaceViewProjections` builds
  `P · lookAtRH(p, ...)` and `lookAtRH(eye, ...) = R · T(−eye)`, so `faceVP(p) = P·R·T(−p)`. The
  blend pass evaluates it at `p + direction`, where the translation cancels outright; the capture
  pass evaluates it at a world position, which is identical to evaluating an origin-built matrix
  at a probe-relative one. What is left is a `float3` position, a tile index and a hysteresis —
  16 B and 8 B. The planned ring was ~200 lines of engine machinery existing to carry a constant
  that provably cancels, and dropping it also removed the per-frame staging buffer and the
  transfer subgraph that fed it. The capture test's existing assertions (face +Z records the
  triangle at distance 2.0, face −Z records nothing) pass unchanged, which is what shows the
  rewrite is equivalent rather than merely plausible.

### 2026-08-01 — VkmScene's second cull view deferred to 4.5
- Planned: extend `VkmScene` with `kVkmSceneMaxCullViews = 2` so a probe capture and a main camera
  could cull with different frusta in one frame.
- Did instead: the updater fills `VkmFrameData::_frustumPlanes` with the AABB of the probes it is
  refreshing and uses the frame's single cull.
- Why: with the GI sample out of scope there is no second view — no consumer for the second path
  and no way to validate it, so it would have been untested engine-core churn. The probe capture's
  cull is a box test regardless, because all six faces share one visible list, so nothing is lost
  in this phase. The planned design also had a real bug worth recording for whoever picks it up:
  `assignBatchRegions` produces two different totals (`visibleCursor`, `argumentCursor`) so there
  is no single per-view stride, and `countWordOffset` indexes *both* the visible-list and argument
  buffers, so it cannot take either. Packing every view's count words at the front of both buffers
  and striding only the payload cursors is the fix. Logged in `TODO.md`.

### 2026-08-01 — Probe resources release immediately at destroy, not through the reclaimer
- Planned: route `VkmProbeVolume` and `VkmProbeVolumeUpdater` releases through
  `getDeferredReclaimer()->requestRelease()`, since a per-frame consumer makes
  destroy-while-in-flight reachable.
- Did instead: release straight to the resource pool, matching `VkmGBuffer::destroy()` — which
  passes `deferred = false` and reserves the reclaimer for `resize()`.
- Why: the reclaimer only frees once the GPU timeline passes the frames that referenced a
  resource, so anything handed to it at shutdown — when nothing further will ever be submitted —
  is still outstanding when the allocator tears down. Vulkan caught it immediately (`vk_mem_alloc.h`
  asserts "Some allocations were not freed", listing three unfreed buffers); Metal did not, which
  is why the Debug Metal suite had been passing. Neither class has a resize path, so this is their
  only release, and the drain-before-destroy contract is the one every other resource owner here
  already has.

### 2026-08-01 — Two VkmScene leaks in the probe tests
- Planned: nothing — this was not anticipated.
- Did instead: added `scene.destroy(driver)` to the new updater fixture *and* to the pre-existing
  `runProbeCaptureTest`.
- Why: `~VkmScene()` is defaulted, so a scene's buffers only return to the pool on an explicit
  `destroy(driver)`, and only `TestSceneModelRenderShared.hpp` was doing it. The pre-existing leak
  in `runProbeCaptureTest` was invisible because that test is Metal-only; it would abort the moment
  it ran on Vulkan. Fixed rather than merely reported, because the same abort is what this change
  had to diagnose and leaving the second instance in place would hand the next person the identical
  hour-long trail — the VMA assert fires inside a signal handler and recurses through backward-cpp,
  so it presents as a hang rather than as a leak (`TODO.md` documents that recursion).

## 2026-08-01 — Wall-clock watchdog in scripts/run_tests.py

`--test-timeout` (default 600 s, `0` disables) kills a backend's `UnitTests` run and reports FAIL,
printing the last 80 lines the process managed to emit.

This is a third layer over the two already in `UnitTests.cpp` (doctest's post-hoc check and the
hang-watchdog thread), and it exists because both of those measure time spent *inside* a test case
and are native-only. Neither can see a hang in fixture construction, in driver teardown after the
last test, or inside a signal handler — and the probe-GI work hit exactly the third case: a VMA
leak assert at Vulkan teardown aborts inside a handler that allocates, which recurses through
backward-cpp and **spins** rather than idling, so it reads as a hung 100%-CPU process with no
diagnosis. It ran 21 minutes before being killed by hand.

Deliberately not wired to the WebGPU path: `run_webgpu_tests_headless_chrome`'s 60 s timeout is how
a *successful* run ends (headless Chrome does not exit on its own; success is the completion marker
in the page output), not a watchdog, so raising it would just make every wasm run take that long.
A comment at that function says so, since "unify the two timeouts" is the obvious wrong cleanup.

Verified both ways: `--test-timeout 1` kills a healthy Metal run and reports FAIL with output
attached, and the default leaves Metal 193/193 and Vulkan 191/191 untouched.

## 2026-08-02 — WebGPU shader compilation unlocked, and the four bugs it was hiding

`restir.md` and `TODO.md` both recorded that WGSL output was unavailable because
`VKM_COMPILER_ENABLE_WGSL=ON` needs a Dawn/tint build "no local or CI configuration provides".
That was wrong on every count: Dawn is vendored and pinned in `bootstrap.json`, the CMake
ExternalProject wiring already existed, `buildscripts/ShaderCompile.cmake` already accepted a
`VKM_HOST_VKM_COMPILER`, and `scripts/run_sample.py` already built one. Only `run_tests.py` did not.

- `scripts/run_tests.py` gained `build_host_vkm_compiler()` (ported from run_sample.py, which it
  already duplicates most helpers with -- noted in TODO.md) and passes the result to the emcmake
  configure.
- `tests/CMakeLists.txt`: the engine PSO dir and cache are now defined for WebGPU too, at their
  MEMFS paths; test-owned caches get their own `--preload-file` mount; the per-pass shader target
  builds for WebGPU when a host compiler exists.
- A `WebGPUPerPassFixture` registers the set-2 tests on that backend.

**The four bugs.** None was reachable without actually running a shader there:

1. `Sample()` in non-uniform control flow. WGSL allows `textureSample` only under uniform control
   flow, and `probe_lighting` samples from a loop with `continue`s while both it and
   `deferred_lighting` sample after an early return. Fixed by `SampleLevel(..., 0)` -- every texture
   involved has one mip, so the implicit LOD could only ever have been 0. (Committed separately.)
2. Staging buffers were created `mappedAtCreation` and left that way. WebGPU rejects any GPU or
   queue use of a mapped buffer, so the first operation on every fresh staging buffer was invalid:
   a readback is copied into before it is read, and `writeDirect()` is a queue write. That broke
   `readbackTexture` and every `VkmScene` upload on that backend. Now created unmapped.
   `map()` also had to switch to `wgpuBufferGetConstMappedRange` for read-only mappings.
3. The compute path never bound bind group 1. The graphics path did, with a comment explaining that
   WebGPU requires every group in the pipeline layout to be set -- the compute path returned early
   before reaching it.
4. Every unpublished bindless singleton bound the *same* placeholder buffer. WebGPU rejects a bind
   group whose writable storage bindings overlap, and separately rejects one buffer used both
   read-write and read-only in a synchronization scope. Distinct slices fix only the first, so each
   singleton now has its own placeholder. This invalidated every WebGPU pass that did not populate
   a scene -- i.e. most of what Phase 4 will do there.

**Still open.** `runPerPassResourceTest` is skipped on WebGPU: `newBuffer` returns null for its
256-byte storage buffer, with no Dawn validation error and no engine log. It had reached dispatch
and readback in earlier iterations, so everything upstream works; the cause is unidentified.
The validation half of the same test runs and passes.

Verified: Metal 193/193 and Vulkan 191/191 Debug, 192/192 and 191/191 Release, WebGPU green.

## 2026-08-02 — Window resize (suspend during, rebuild after)

Nothing reacted to a window resize before this: `VkmSwapChainBase::resize()` was a TODO stub with
zero callers, `-[VkmApplicationImpl windowDidResize:]` was empty, and no GLFW framebuffer-size
callback existed anywhere.

- **Handoff.** Resize events arrive on the window thread, which on the macOS Metal path is not the
  thread running `loopInner()`. `VkmWindowContext` gained two atomics — `_pendingExtent` (packed
  `(width << 32) | height` plus a bit-63 marker, so a genuine 0x0 from a minimize is
  distinguishable from "nothing pending") and `_liveResizeActive`. The window thread only
  publishes; `VkmEngine::render()` consumes. No driver call ever happens on the window thread.
  `_windowContexts` became a `std::deque` because atomics make the context non-movable.
- **Drain.** `VkmEngine::recreateSwapChain()` does `ensureCompleted()` on all `FRAME_COUNT` graphs
  of that window, then `VkmDriverBase::waitIdle()` (every queue, so uploads and readbacks too),
  then `resize()`. The swapchain releases its back buffers immediately rather than through the
  deferred reclaimer, so that drain is what makes it safe.
- **Suspension.** AppKit's `windowWillStartLiveResize:`/`windowDidEndLiveResize:` bracket the drag
  exactly; the engine renders nothing in between and `kCAGravityResizeAspect` scales the last
  presented frame into the new bounds. GLFW has no such bracket. Its size callback only fires from
  `glfwPollEvents()`, which runs once per frame, so a frame's events coalesce into at most one
  rebuild; Win32 and Cocoa additionally run a modal resize loop that `glfwPollEvents()` does not
  return from, so a drag there draws nothing at all, while X11 rebuilds once per frame. Both are
  correct — only the first also freezes rendering.
- **Out-of-date self-heal.** Vulkan `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR` from acquire or
  present, and WebGPU `Outdated`/`Lost`, now set `_outOfDate` instead of being logged as errors or
  ignored. The engine rebuilds on the next frame, which covers resizes no window callback reported.
- **macOS drawable size.** `CAMetalLayer.drawableSize` was hardcoded to 1920x1080 (and 960x640 for
  the ImGui window) regardless of window size, so a 1280x720 window rendered at 1080p and was
  letterboxed. It now tracks `[view convertSizeToBacking:view.bounds.size]` at creation and on
  every resize, i.e. the window's true pixel resolution.
- **Vulkan extent write-back.** `createSwapChain` created images from
  `surfaceCapabilities.currentExtent` but left `_extent` at whatever the platform layer requested,
  so `getExtent()` — which feeds the camera aspect and every framebuffer descriptor — was wrong on
  any HiDPI display. It now writes the real extent back, and handles the `0xFFFFFFFF` "you choose"
  sentinel by clamping the requested size to `min/maxImageExtent`.
- **Input.** Absolute cursor positions need no correction (they are window-relative and nothing
  caches a window size), but the *deltas* do: dragging a window's left or top edge moves its origin,
  so the cursor's window-relative position jumps without the pointer moving. A new
  `VkmInputEventType::WindowResize` goes through the same mutex-buffered queue as the cursor moves,
  so it stays ordered against them, and drops the tracked baseline in `VkmInputHandler` and in both
  camera controllers — the same thing they already do on focus loss, for the same reason.
- **Samples.** `triangle` (800x600) and `empty_screen` (1280x720) hardcoded
  `frameBufferDesc._width/_height`; both now read `getSwapChain(windowIndex)->getExtent()` like
  `model_viewer` and `skybox` already did. Removed the matching TODO.md clause.

### Deviations

- Planned: only `VkmSwapChainBase::resize()`, the platform hooks, and the sample fixes.
  Did instead: also (a) reset `_backBuffers` entries to invalid in `destroySwapChainCommon()`, since
  a backend may produce fewer images on recreate than it did before and a leftover handle would be
  double-released; (b) set `_backBufferCount` on Metal and WebGPU, which never did, and allocated
  `BACK_BUFFER_COUNT` placeholder textures there instead of `MAX_BACK_BUFFER_COUNT` — otherwise the
  new `getBackBuffer()` accessor reports nothing on two of three backends and every resize would
  churn five unused textures. Both are required for resize to be correct rather than optional
  cleanups.
- Planned: nothing about ImGui. Did instead: added `VkmImGuiRendererBase::discardFrame()`.
  `ImGui::Render()` is only reachable through `renderDrawData()`, so any frame that skips the ImGui
  window leaves an unclosed ImGui frame and the next `ImGui::NewFrame()` asserts. Suspension made
  that reachable constantly; the existing failed-acquire `continue` could already trip it.
- Planned: nothing about the display-link callback. Did instead: it now skips
  `[_imguiMetalLayer nextDrawable]` while that window is suspended. Drawables that are never
  presented drain the 3-deep pool, after which every `nextDrawable` blocks out its full timeout and
  stalls the render thread for the whole drag.
- Planned: `recreateSwapChain()` would drain and then call `resize()`, letting `resize()`'s
  same-extent early-out handle the no-op case. Did instead: the same-extent check was hoisted into
  `recreateSwapChain()` ahead of the drain. Interactive testing showed the original shape paying
  for a full `waitIdle()` and logging "Swapchain recreated" twice at startup, where the platform
  layer's initial publish reports exactly the size the swapchain was already built at. `resize()`
  keeps its own guard for direct callers.

### Verification

- `scripts/run_tests.py`: metal / vulkan / webgpu all PASS. Metal: 193 test cases / 19188
  assertions under `MTL_DEBUG_LAYER=1`; Vulkan: 191 / 3928 with validation layers on.
- New Vulkan coverage in `tests/TestEngineSetup.cpp`, validation layers on: `VkmSwapChainVulkan`
  resize / no-op resize / zero-extent teardown-and-restore, and a `VkmEngine` test driving the
  real publish -> suspend -> drain -> rebuild -> minimize -> restore sequence through
  `loopInner()`.
  Note that the latter absorbed the former "ImGui renderer survives one loopInner() tick" test
  rather than sitting alongside it: `initializeEngine()` registers process-wide loggers, so a
  second `VkmEngine` in the same process throws "logger with name 'ConsoleLogger' already
  exists". There can be exactly one live-engine test per binary, and this is it.
- Interactive, both backends, via AppleScript-driven window resizes of the `triangle` sample:
  the swapchain rebuilds at the correct backing-scaled pixel size (900x700 pt -> 1800x1336 px on a
  2x display), with zero Metal API Validation and zero Vulkan validation-layer output. On Metal the
  log confirms the rebuild runs on the render thread while window setup ran on the main thread,
  which is the handoff this change exists for.
- Not verified by me: an actual mouse-driven live-resize drag, i.e. the
  `windowWillStartLiveResize:`/`windowDidEndLiveResize:` suspension path on Metal. A programmatic
  resize reports `inLiveResize == NO` and goes through `windowDidResize:` instead, so the brackets
  themselves need a human dragging a window edge.
## 2026-08-02 — VkmScene: two cull views per frame

Deferred out of 4.2c because it had no consumer then; 4.5 gives it one. A probe capture sees in
every direction, so culling it against the camera frustum drops exactly the geometry behind the
camera that indirect light comes from -- a GI frame has to cull twice, against different frusta.

`kVkmSceneMaxCullViews = 2`. `recordUpdate`/`recordCull`/`recordDrawBatches` take a `viewIndex`
(default 0, so every existing caller is unchanged), and `SceneBatchConstants` carries a
`frameDataIndex` that `scene_cull.hlsl` indexes the frame data with.

Two layout details carry the design:

- **All views' count words sit at the front of both buffers**, not beside each view's payload. A
  batch uses ONE index into the visible-list and argument buffers, and their payloads have
  different strides (`assignBatchRegions` produces two different cursors), so no single per-view
  offset would serve both. Counts are `viewIndex * batchCount + i`; the payloads get their own
  strides.
- **Each view needs its own staging region, not just its own device region.** Both `recordUpdate`
  calls write host memory immediately, long before either GPU copy runs, so a shared staging slot
  leaves the first cull reading the second view's frustum. This was the actual blocker all along.

`SceneBatchConstants`'s padding also changed from `uint3` to scalars: WebGPU lowers push constants
to a uniform buffer, where a vector aligns to its own size and would land at a different offset
than in the scalar layout Vulkan and Metal use. The used fields were all below the padding, so
nothing was broken -- but adding a field right above it would have been.

New `tests/TestSceneCullViewsShared.hpp` culls one object against a seeing and a blind frustum in
one frame and checks the two counts. Both failure modes were confirmed by sabotage, and they fail
*different* assertions: forcing `g_FrameData[0]` makes view 1 report 1, and sharing the staging
region makes view 0 report 0. Runs on Metal and Vulkan.

Metal 194/194 and Vulkan 192/192 Debug, 193/193 and 192/192 Release, WebGPU green.

## 2026-08-02 — 4.5: the GI sample, and the shared composite

`src/samples/gi` is the first application to drive the deferred chain, so it is also where Phase 3's
gate ("G-buffer channels visualizable via a debug view") finally gets settled.

Per frame: probe refresh (cull view 1) -> scene update and cull (view 0) -> G-buffer -> deferred
lighting -> probe lighting -> composite -> tone map, with `barrierTextureForShaderRead` between each
render-target-then-sampled hand-off.

- **The sample owns no shaders.** Every pass is an engine PSO, which is what lets it build on WebGPU
  without a per-sample WGSL cache -- it needs only the engine's MEMFS preload.
- **New shared pass `gi_composite`** (`resources/Shaders/gi_composite.hlsl`,
  `include/vkm/renderer/gi_composite.h`). This is the consuming end of the technique interface §5
  describes: the only place a technique's output is combined (irradiance x albedo / pi, added to
  direct), so a second technique means adding passes rather than editing this. It also carries the
  ten debug views, since it is the pass that already has every G-buffer channel bound.
- **The probe grid is fitted to `VkmScene::computeWorldBounds()`** rather than guessed, with a 20%
  margin so edge surfaces sit between probes instead of outside the grid, where the lookup is black.
- **The probe budget is derived, not fixed.** The capture pushes once per (probe, face, batch) and
  the Metal/WebGPU push-constant ring has no per-frame reset, so a fixed budget wraps the ring onto
  entries a running frame still references -- which the first Sponza run did, 365 times. The sample
  now computes a budget from the draw-batch count. `kVkmPushConstantRingEntryCount` moved to the
  common bindless header for this, since it is a budget callers have to plan against rather than a
  backend detail.
- Per-frame composite settings ride a staging buffer per frame slot, mirroring `VkmScene`: the table
  binding the uniform buffer stays immutable while its contents change, which is how a per-pass
  table is meant to be used. Resizing rebuilds every table, and the old ones wait out
  FRAME_BUFFER_COUNT frames rather than being deleted under a running GPU.

**Verification.** `vkmWriteTexturePng` (new, `renderer/screenshot.{h,cpp}`) reads a colour texture
back and writes a PNG; the sample drives it with `--gv_gi_screenshot=<png>`,
`--gv_gi_screenshot_frame=<n>` and `--gv_gi_debug_view=<n>`, tone-mapping a second time into an
owned target on the capture frame and exiting once the file is written. The backbuffer itself
cannot be the source -- Metal keeps `framebufferOnly = YES` on the drawable. `VkmInputHandler`
gained a `requestExit()` so a screenshot run can stop itself.

That is what verifies any of this without a display, and the indirect-only view over Sponza settles
the question the atlas tests could not reach: it shows real colour bleeding, green where light
bounced off the green material and grey where it did not, so the atlas the updater writes *is*
addressed the way `probe_lighting` reads it. Still no *automated* pixel check -- screenshots are
compared by eye (`TODO.md`).

A correction to an earlier claim in this session: a Sponza run that appeared to render cleanly for
35 s was mostly spent importing the scene, and a later "built" check passed only because the grep
looked for "error" and the failure said "No rule to make target" -- the build directory had been
reconfigured with BUILD_SAMPLES=OFF, so several screenshot runs used a stale binary.

Metal 194/194 and Vulkan 192/192 Debug, 193/193 and 192/192 Release.
