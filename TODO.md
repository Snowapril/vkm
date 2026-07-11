# TODO.md

## Known Limitations

- Vulkan sample builds fail to link on macOS (`application.mm` hard-codes Metal driver/swapchain types).
- PSO JSON file-based unit tests skip under Emscripten (no RESOURCES_DIR fixture access in the wasm virtual filesystem).
- Vulkan `UnitTests` fail at runtime in GitHub Actions CI on all platforms (`vkCreateInstance` fails: no working Vulkan-capable GPU/driver on the hosted runners); passes locally with a real GPU. Needs a software Vulkan ICD (e.g. lavapipe/SwiftShader) installed in CI to actually exercise this backend there.
- Implement backend pipeline object creation (Vulkan/Metal/WebGPU) consuming `VkmPipelineStateDescriptor`; parsing only exists today.
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
