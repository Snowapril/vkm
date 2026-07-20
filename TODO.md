# TODO.md

## Known Limitations

- Vulkan `UnitTests` in GitHub Actions CI still lack a software Vulkan ICD on the Windows and macOS runners (Ubuntu now installs lavapipe).
- `MemoryTracker`'s global mutex serializes every allocation/deallocation across all threads.
- Design and implement descriptor sets 1-3 of the engine/user resource-binding convention (set 0 bindless is implemented on all backends).
- The common command-buffer interface has no compute dispatch entry point; Metal's `beginComputePass` scaffolding is never invoked.
- `MTL4RenderPipelineDescriptor` has no depth/stencil attachment format properties in Metal4; `VkmPipelineStateMetal` can't validate/set depth-stencil format at pipeline-creation time, only at render-pass/encoder time.
- Migrate vkm's own engine shaders (scene_object/tonemap/etc.) from loose GLSL to HLSL+PSO json so `resources/Pipelines/Engine/` and the `vkm_engine_shaders` CMake target actually have something to compile.
- WebGPU resources report `getAllocatedSize()`/`getMemoryAlignment()` as a passthrough of the requested size (wgpu has no allocation introspection; Vulkan/Metal report real values).
- PSO JSON has no push-constant/descriptor-set representation; the bindless set 0 layout and push-constant range are hardcoded per backend (VkmPipelineStateVulkan::createInner, vkm-compiler's MSL remaps, VkmBindlessResourceManagerWebGPU's bind group layout).
- `getProcessCpuUsagePercent()` returns 0 on wasm.
- wasm builds bake the on-disk `RESOURCES_DIR` into the engine library and samples; the MEMFS `--preload-file` remap covers only the `UnitTests` and `triangle` targets.
- Metal draw-time encoder state (fill mode, cull mode, front face) from the PSO descriptor is never applied, so the wireframe PSO variant renders solid on Metal.
- WebGPU bindless mega-buffers are fixed-capacity (16 MiB vertex / 8 MiB index) with no growth; registerBuffer fails hard when exhausted.
- WebGPU bindless-registered buffers must be tightly packed engine VertexData/uint element arrays (typed mega-buffers; Vulkan/Metal treat them as opaque).
- The Metal/WebGPU push-constant ring wraps after 1024 allocations with no per-frame reset; overlapping in-flight entries would be overwritten.
- wasm.yml CI builds no WebGPU shader caches (needs a native host vkm-compiler + tint/dawn build wired in with an actions cache).
- Extend `VkmResourcePoolType` with Graphics/Compute categories for narrower Metal residency sets.
- Metal resources bound via `overrideExternalHandle()` after creation never enter a residency set.
- `VkmRenderResourcePool`'s constructor takes a `VkmDriverBase*` but never stores it in a member.
- Metal `releaseResource` stages `removeAllocation:` for swapchain backbuffers that were never added to the residency set.
- `VkmGpuCrashHandler` breadcrumbs are per-submission only; `VK_NV_device_diagnostic_checkpoints` (per-draw-call attribution on NVIDIA) was deliberately not implemented.
- `VkmCommandBufferBase::setDebugName()` is never called by the render graph today, so crash-handler breadcrumbs always fall back to their auto-generated `"<queueName>#<index>"` name.
- Sporadic `MTL4CommandQueueErrorTimeout` feedback errors observed on the Metal4 triangle sample even without the crash-dump flag; frequency environment-dependent, root cause not yet investigated.
- `VkmGpuCrashHandler::clearFrameMarkers()` blocks on the graphics queue's `waitIdle()` every frame while `--enable-gpu-crash-dump` is set, since no per-frame-slot completion wait exists in the live render loop.
- `copyTexture`/`copyTextureToBuffer` (and thus render graph content capture / `readbackTexture`) are Metal-only; Vulkan/WebGPU have error-logging stubs.
- Render graph capture records depth/stencil attachments as metadata only (no snapshot/preview).
- Render graph capture records swapchain backbuffer outputs as metadata only (`CAMetalLayer.framebufferOnly` stays YES).
- Render graph capture texture previews in ImGui are Metal-only (`getTextureID` returns 0 on Vulkan/WebGPU).
- Programmatic .gputrace capture scopes only the Metal Graphics queue 0; Vulkan/WebGPU `requestGpuFrameCapture()` is a no-op (no RenderDoc integration).
