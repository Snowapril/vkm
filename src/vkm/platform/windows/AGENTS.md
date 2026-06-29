# Windows Platform Layer

Target: **Windows Vulkan** — `VKM_PLATFORM_WINDOWS=ON` + `VKM_USE_VULKAN_API=ON`.

## File Conventions

- Extensions: `.h` / `.cpp` — pure C++20
- Guard platform-specific code with `#ifdef VKM_PLATFORM_WINDOWS`
- Corresponding headers in `include/vkm/platform/windows/`; update both together

## Window Handle

```cpp
// include/vkm/platform/common/window.h
#ifdef VKM_PLATFORM_WINDOWS
    using VkmNativeWindowHandle = GLFWwindow*;
#endif
```

All window handles passed through the system are `GLFWwindow*`. Do not use Win32 `HWND` directly; extract it from GLFW when needed for Vulkan surface creation:
```cpp
HWND hwnd = glfwGetWin32Window(glfwWindow);
```

## GLFW Event Loop

The application class (`application.h` / `application.cpp`) owns the GLFW window and drives the main loop:
- `glfwPollEvents()` — called each frame
- Window resize → `VkmSwapChainBase::resize(width, height)`
- Window close → trigger engine shutdown

Do not call `glfwInit` / `glfwTerminate` more than once per process.

## Vulkan Surface

GLFW provides `glfwCreateWindowSurface(instance, window, nullptr, &surface)` which handles the Win32 surface extension internally. Prefer this over manually calling `vkCreateWin32SurfaceKHR`.

## Backend Pairing

This platform layer is **always paired with the Vulkan backend**. When working here also read:
`src/vkm/renderer/backend/vulkan/AGENTS.md`

## Implementation Checklist

- [ ] `VKM_PLATFORM_WINDOWS` guard around Win32-specific includes (`<windows.h>`)
- [ ] GLFW window created with `GLFW_CLIENT_API = GLFW_NO_API` (no OpenGL context)
- [ ] Window resize callback registered and forwarded to swapchain
- [ ] `glfwDestroyWindow` called before `glfwTerminate`
