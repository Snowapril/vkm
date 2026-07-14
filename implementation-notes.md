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
