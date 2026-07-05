# WebGPU Backend

Targets: **Emscripten/WASM only**, running inside a Chromium-based browser. `VKM_USE_WEBGPU_API` + `VKM_PLATFORM_WASM`. No native desktop WebGPU target (no Dawn/wgpu-native) — out of scope.

## File Conventions

- Extensions: `.h` / `.cpp` — pure C++20, no Objective-C (WebGPU's `webgpu.h` is a C API, unlike Metal it needs no header-hiding tricks)
- Guard all code in this directory with `#ifdef VKM_USE_WEBGPU_API`
- Corresponding headers live in `include/vkm/renderer/backend/webgpu/`; update both together

## WebGPU API Access

Always use `<webgpu/webgpu.h>`, supplied by Emscripten's **`emdawnwebgpu`** port:
```cmake
# root CMakeLists.txt
set(VKM_EMSCRIPTEN_COMPILE_FLAGS "SHELL:--use-port=emdawnwebgpu")
```
Never vendor a copy of `webgpu.h`. Do not use the deprecated `-sUSE_WEBGPU=1` flag or the legacy `emscripten_webgpu_get_device()` helper — both predate the current Future/CallbackInfo-based API this backend is written against.

## Async Initialization (Asyncify)

`wgpuInstanceRequestAdapter` / `wgpuAdapterRequestDevice` are callback-based and resolve asynchronously in the browser. `VkmDriverWebGPU::initializeInner` must still return synchronously (base signature is frozen — see `common/AGENTS.md`), so it bridges this with a bounded spin using `emscripten_sleep` (requires `-sASYNCIFY`) while a `WGPUCallbackMode_AllowSpontaneous` callback flips a local `bool` flag:
```cpp
wgpuInstanceRequestAdapter(_instance, &options, callbackInfo);
while (!adapterRequestDone) { emscripten_sleep(1); }
```
Follow this same pattern for any other WebGPU call that only exposes a callback/Future API.

## Command Encoder ≠ Command Buffer

WebGPU has no pre-allocated "command buffer" step the way Vulkan/Metal do. `VKM_COMMAND_BUFFER_HANDLE` maps to a `WGPUCommandEncoder` from creation until `wgpuCommandEncoderFinish` is called — that call happens inside `VkmCommandQueueWebGPU::submit`, right before `wgpuQueueSubmit`, not inside the command buffer class itself. Do not try to "finish" the encoder anywhere else.

## Class Override Map

- `VkmDriverWebGPU` — `initializeInner` (instance/adapter/device/queue via Asyncify), `destroyInner` (reverse-order release), `newTextureInner` → `VkmTextureWebGPU`, `newSwapChainInner` → `VkmSwapChainWebGPU`, `newCommandQueueInner` → `VkmCommandQueueWebGPU`
- `VkmSwapChainWebGPU` — `createSwapChain` obtains a `WGPUSurface` from the canvas via `WGPUEmscriptenSurfaceSourceCanvasHTMLSelector` (selector must match the actual canvas element id — see `platform/wasm/AGENTS.md`), queries `wgpuSurfaceGetCapabilities` for the preferred format (do not hardcode a format), `acquireNextImageInner` calls `wgpuSurfaceGetCurrentTexture` and overrides a rotating backbuffer slot (mirrors Metal's `overrideCurrentDrawable` pattern), `presentInner` calls `wgpuSurfacePresent`
- `VkmTextureWebGPU` — `overrideExternalHandle` takes ownership-free of swapchain-provided `WGPUTexture`s (tracked via an `_externallyOwned` flag so the destructor skips `wgpuTextureRelease`)
- `VkmCommandQueueWebGPU` / `VkmCommandBufferPoolWebGPU` / `VkmGpuEventTimelineWebGPU` — see below
- `VkmCommandBufferWebGPU` — `onBeginRenderPass` builds `WGPURenderPassColorAttachment`s from texture views created just for the call and released immediately after `wgpuCommandEncoderBeginRenderPass` returns; `onEndRenderPass` calls `wgpuRenderPassEncoderEnd` + release

## GPU Timeline Without a Native Fence

WebGPU exposes no timeline semaphore or shared-event equivalent. `VkmGpuEventTimelineWebGPU` tracks completion purely via `wgpuQueueOnSubmittedWorkDone` callbacks updating an atomic counter:
- `queryLastCompletedTimeline` — reads the cached atomic; **eventually consistent**, not a synchronous GPU query like Vulkan's `vkGetSemaphoreCounterValue`
- `waitIdle(timeoutMs)` — a **best-effort bounded poll** via `emscripten_sleep`, not a true blocking wait; document this at any new call site, don't silently treat it as equivalent to Vulkan/Metal's wait

## Implementation Checklist

- [ ] Encoder finished (`wgpuCommandEncoderFinish`) immediately before `wgpuQueueSubmit`, never before
- [ ] `uncapturedErrorCallbackInfo` registered on the device descriptor (not a separate setter — it's a descriptor field in this API revision)
- [ ] No blocking waits anywhere outside an Asyncify-compiled (`-sASYNCIFY`) call path
- [ ] Every `wgpuXInstanceCreateSurface`/format/present-mode choice checked against `wgpuSurfaceGetCapabilities` rather than assumed
- [ ] No `vulkan.h`/`Metal.h` includes — this backend is WebGPU-only
