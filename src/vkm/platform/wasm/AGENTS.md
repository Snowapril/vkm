# WASM Platform Layer

Target: **Emscripten/WASM WebGPU**, running inside a Chromium-based browser. `VKM_PLATFORM_WASM=ON` + `VKM_USE_WEBGPU_API=ON`. Compiled from Ubuntu, macOS, or Windows hosts via emsdk — the host OS is not the runtime target, the browser is.

## File Conventions

- Extensions: `.h` / `.cpp` — pure C++20
- Guard platform-specific code with `#ifdef VKM_PLATFORM_WASM`
- Corresponding headers in `include/vkm/platform/wasm/`; update both together

## Window Handle

```cpp
// include/vkm/platform/common/window.h
#elif defined(VKM_PLATFORM_WASM)
#define VKM_WINDOW_HANDLE GLFWwindow*
```
Reuses GLFW via Emscripten's built-in GLFW3 port (`-sUSE_GLFW=3`) rather than a raw HTML5 canvas selector — the port hands back a real (emulated) `GLFWwindow*`, so the Windows platform layer's pattern applies almost unchanged. The canvas selector the WebGPU backend's swapchain uses to obtain a `WGPUSurface` (see `renderer/backend/webgpu/AGENTS.md`) must match whatever DOM element id the GLFW port binds to (`"#canvas"` by default) — if a custom `--shell-file` is ever introduced, keep the canvas `id="canvas"`.

## Main Loop — Never Block

This is the single most important difference from every other platform layer. Emscripten forbids blocking the browser's main thread: a `while (!shouldClose()) { ... }` loop like Windows/Apple use would freeze the tab and also prevent the WebGPU backend's Asyncify-based callbacks from ever firing (they resolve via the browser event loop).

Instead, register a callback-driven loop and return immediately:
```cpp
emscripten_set_main_loop_arg(&VkmApplication::mainLoopTrampoline, this, 0, 1);
```
With `simulate_infinite_loop=1`, this call does not return in the normal sense — control unwinds back to the browser event loop. Any cleanup (`_engine.destroy()`, `glfwTerminate()`) must happen from inside the trampoline once `shouldClose()` is true, not after this call.

## Backend Pairing

Always paired with the WebGPU backend. When working here also read:
`src/vkm/renderer/backend/webgpu/AGENTS.md`

## Implementation Checklist

- [ ] No blocking `while` loop anywhere in this directory — always structure per-frame work as a callback registered via `emscripten_set_main_loop_arg`
- [ ] `glfwInit()` called exactly once; `glfwTerminate()` called from the main-loop trampoline on close, not after `entryPoint` returns
- [ ] Canvas resize forwarded to `VkmSwapChainBase::resize` via `emscripten_set_resize_callback` (not yet wired — flag as a gap if adding resize support)
- [ ] `glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)` set before window creation (no GL context)
