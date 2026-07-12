# TODO.md

## Known Limitations

- Vulkan sample builds fail to link on macOS (`application.mm` hard-codes Metal driver/swapchain types).
- PSO JSON file-based unit tests skip under Emscripten (no RESOURCES_DIR fixture access in the wasm virtual filesystem).
- Vulkan `UnitTests` fail at runtime in GitHub Actions CI on all platforms (`vkCreateInstance` fails: no working Vulkan-capable GPU/driver on the hosted runners); passes locally with a real GPU. Needs a software Vulkan ICD (e.g. lavapipe/SwiftShader) installed in CI to actually exercise this backend there.
- Metal4 ImGui backend has no keyboard or scroll-wheel input yet, only polled mouse position/buttons.
- Vulkan swapchain present/acquire uses a fence + vkDeviceWaitIdle instead of per-frame semaphores (correct but not pipelined).
- `MemoryTracker::allocate()` doesn't null-check mimalloc/std::malloc, causing a null-pointer write on OOM instead of throwing `std::bad_alloc`.
- `LoggerManager` singleton has the same mimalloc-shutdown-ordering hazard `MemoryTracker` was fixed for; never made immortal or explicitly closed before exit.
- `MemoryTracker`'s global mutex serializes every allocation/deallocation across all threads.
- `VKM_NEW_TAGGED`'s label static-storage-duration requirement isn't compiler-enforced.
- mimalloc-vs-WASM fallback branching is duplicated across `memory.cpp`; `usableBytes` semantics have already diverged per platform.
- Global `operator new`/`delete` overrides in `memory.cpp` duplicate the same body across six functions.
- `MemoryTracker::~MemoryTracker()` is dead code since `singleton()` never destructs the instance.
- `MemoryTracker::getMimallocStats()` declares unused locals instead of passing `nullptr` to `mi_process_info`.
- Design and implement the 8-set engine/user resource-binding (descriptor set) convention on top of `VkmPipelineStateBase`.
- Implement draw-call recording (vertex buffer binding, dynamic viewport/scissor, draw calls) — pipeline objects can now be created but nothing draws yet.
- `VkmCommandBufferWebGPU::onBindPipeline` no-ops with a logged error for compute pipelines since no compute pass encoder is tracked yet.
- `MTL4RenderPipelineDescriptor` has no depth/stencil attachment format properties in Metal4; `VkmPipelineStateMetal` can't validate/set depth-stencil format at pipeline-creation time, only at render-pass/encoder time.
- Migrate vkm's own engine shaders (scene_object/tonemap/etc.) from loose GLSL to HLSL+PSO json so `resources/Pipelines/Engine/` and the `vkm_engine_shaders` CMake target actually have something to compile.
- `VkmPipelineStateManager::getPipelineState(name)`'s Engine-wins-on-collision precedence over User origin is an unrevisited default, not a deliberate policy.
- `metal_pipeline_state.mm`'s `getMTLPixelFormat` duplicates `metal_texture.mm`'s anonymous-namespace version instead of sharing a common conversion helper.
- Vulkan `vkCreateDevice` on macOS/MoltenVK logs a validation error (`VUID-VkDeviceCreateInfo-pProperties-04451`) for not enabling `VK_KHR_portability_subset`, which the physical device reports as supported.
- Metal/WebGPU resources report `getAllocatedSize()`/`getMemoryAlignment()` as a best-effort passthrough of the requested size, not a real backend-reported number (only Vulkan's VMA path gives a real one).
- `VkmResourceHandle::generation` is unexercised scaffolding: `VkmRenderResourcePool` never recycles resource IDs, so a generation mismatch can never actually occur today.
- Extend `VkmResourcePoolType` with Graphics/Compute categories for narrower Metal residency sets.
- Metal resources bound via `overrideExternalHandle()` after creation never enter a residency set.
- `VkmRenderResourcePool`'s constructor takes a `VkmDriverBase*` but never stores it in a member.
- Metal `releaseResource` stages `removeAllocation:` for swapchain backbuffers that were never added to the residency set.
