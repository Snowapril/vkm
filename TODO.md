# TODO.md

## Known Limitations

- Vulkan sample builds fail to link on macOS (`application.mm` hard-codes Metal driver/swapchain types).
- PSO JSON file-based unit tests skip under Emscripten (no RESOURCES_DIR fixture access in the wasm virtual filesystem).
- Implement backend pipeline object creation (Vulkan/Metal/WebGPU) consuming `VkmPipelineStateDescriptor`; parsing only exists today.
