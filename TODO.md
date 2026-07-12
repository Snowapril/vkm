# TODO.md

## Known Limitations

- Vulkan `UnitTests` in GitHub Actions CI still lack a software Vulkan ICD on the Windows and macOS runners (Ubuntu now installs lavapipe).
- Metal triangle sample fails to build: spirv-cross MSL generation of the bindless `triangle.hlsl` requires argument buffer tier 2.
- `MemoryTracker`'s global mutex serializes every allocation/deallocation across all threads.
- Design and implement descriptor sets 1-3 of the engine/user resource-binding convention (set 0 bindless is implemented, Vulkan-only).
- The common command-buffer interface has no compute dispatch entry point; Metal's `beginComputePass` scaffolding is never invoked.
- `MTL4RenderPipelineDescriptor` has no depth/stencil attachment format properties in Metal4; `VkmPipelineStateMetal` can't validate/set depth-stencil format at pipeline-creation time, only at render-pass/encoder time.
- Migrate vkm's own engine shaders (scene_object/tonemap/etc.) from loose GLSL to HLSL+PSO json so `resources/Pipelines/Engine/` and the `vkm_engine_shaders` CMake target actually have something to compile.
- WebGPU resources report `getAllocatedSize()`/`getMemoryAlignment()` as a passthrough of the requested size (wgpu has no allocation introspection; Vulkan/Metal report real values).
- PSO JSON has no push-constant/descriptor-set representation; the bindless set 0 layout and push-constant range are hardcoded in `VkmPipelineStateVulkan::createInner` instead.
- `VkmCommandBufferMetal`/`VkmCommandBufferWebGPU`'s `onCopyBuffer`/`onDraw`/`onSetPushConstants` are stub-only (log an error, no-op); bindless draw-call recording is Vulkan-only.
- `getProcessCpuUsagePercent()` returns 0 on wasm.
- Extend `VkmResourcePoolType` with Graphics/Compute categories for narrower Metal residency sets.
- Metal resources bound via `overrideExternalHandle()` after creation never enter a residency set.
- `VkmRenderResourcePool`'s constructor takes a `VkmDriverBase*` but never stores it in a member.
- Metal `releaseResource` stages `removeAllocation:` for swapchain backbuffers that were never added to the residency set.
- `VkmGpuCrashHandler` breadcrumbs are per-submission only; `VK_NV_device_diagnostic_checkpoints` (per-draw-call attribution on NVIDIA) was deliberately not implemented.
- `VkmCommandBufferBase::setDebugName()` is never called by the render graph today, so crash-handler breadcrumbs always fall back to their auto-generated `"<queueName>#<index>"` name.
- Sporadic `MTL4CommandQueueErrorTimeout` feedback errors observed on the Metal4 triangle sample even without the crash-dump flag; frequency environment-dependent, root cause not yet investigated.
- `VkmGpuCrashHandler::clearFrameMarkers()` blocks on the graphics queue's `waitIdle()` every frame while `--enable-gpu-crash-dump` is set, since no per-frame-slot completion wait exists in the live render loop.
