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
  backend actually allocated, and `VkmTextureUploadMode` (Auto/ForceStaging/ForceHostCopy)
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

## Deviations

Log entries here when an edge case forces a deviation from an agreed plan. Format:

```
### <date> — <short title>
- Planned: <what the plan said>
- Did instead: <the conservative option taken>
- Why: <the edge case that forced it>
```

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
