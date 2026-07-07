# TODO.md

## Known Limitations

- Vulkan sample builds fail to link on macOS (`application.mm` hard-codes Metal driver/swapchain types).
- PSO JSON file-based unit tests skip under Emscripten (no RESOURCES_DIR fixture access in the wasm virtual filesystem).
- Vulkan `UnitTests` fail at runtime in GitHub Actions CI on all platforms (`vkCreateInstance` fails: no working Vulkan-capable GPU/driver on the hosted runners); passes locally with a real GPU. Needs a software Vulkan ICD (e.g. lavapipe/SwiftShader) installed in CI to actually exercise this backend there.
- Implement backend pipeline object creation (Vulkan/Metal/WebGPU) consuming `VkmPipelineStateDescriptor`; parsing only exists today.
- Metal4 ImGui backend has no keyboard or scroll-wheel input yet, only polled mouse position/buttons.
- Vulkan swapchain present/acquire uses a fence + vkDeviceWaitIdle instead of per-frame semaphores (correct but not pipelined).
