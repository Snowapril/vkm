# Implementation Notes

Running log of implementation work in this repo. Updated while a plan is being carried out;
see `CLAUDE.md` §11 for the policy.

## Deviations

Log entries here when an edge case forces a deviation from an agreed plan. Format:

```
### <date> — <short title>
- Planned: <what the plan said>
- Did instead: <the conservative option taken>
- Why: <the edge case that forced it>
```

### 2026-07-15 — PR #18 CI fixes: wasm stamp dependency and DXC vs new AppleClang
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
