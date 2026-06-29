# Vulkan Backend

Targets: **Windows Vulkan** (`VKM_PLATFORM_WINDOWS`) and **macOS Vulkan** (`VKM_PLATFORM_APPLE` + `VKM_USE_VULKAN_API`).

## File Conventions

- Extensions: `.h` / `.cpp` — pure C++20, no Objective-C
- Guard all code in this directory with `#ifdef VKM_USE_VULKAN_API`
- Corresponding headers live in `include/vkm/renderer/backend/vulkan/`; update both together

## Vulkan API Access

**Use volk, never include `vulkan.h` directly:**
```cpp
#include <volk.h>   // correct
// #include <vulkan/vulkan.h>  // wrong
```
volk provides dynamic function loading; direct Vulkan header bypasses it.

## Memory Allocation

Use VMA for all GPU memory. Never call `vkAllocateMemory` directly:
```cpp
#include <vk_mem_alloc.h>
// VmaAllocator is owned by VkmDriverVulkan
```

## Device Feature Chain Pattern

Features are linked via pNext before `vkCreateDevice`. Follow the existing pattern in `vulkan_driver.cpp`:
```cpp
VkPhysicalDeviceFeatures2        _deviceFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
VkPhysicalDeviceVulkan11Features _features11{.sType = ...};
VkPhysicalDeviceVulkan12Features _features12{.sType = ...};
VkPhysicalDeviceVulkan13Features _features13{.sType = ...};
// chain: _deviceFeatures.pNext = &_features11; etc.
```
Do not query features before calling `vkGetPhysicalDeviceFeatures2`.

## Class Override Map

`VkmDriverVulkan` overrides:
- `initializeInner` — VkInstance, VkPhysicalDevice, VkDevice creation
- `destroyInner` — cleanup in reverse order
- `newTextureInner` — returns `new VkmTextureVulkan`
- `newSwapChainInner` — returns `new VkmSwapChainVulkan`
- `newCommandQueueInner` — **not overridden** → base class handles command queue creation

## Platform-Specific Surface Creation

```cpp
#ifdef VKM_PLATFORM_WINDOWS
    // VK_USE_PLATFORM_WIN32_KHR defined by volk on Windows
    // use vkCreateWin32SurfaceKHR or GLFW's glfwCreateWindowSurface
#endif
#ifdef VKM_PLATFORM_APPLE
    // MoltenVK / VK_MVK_macos_surface or VK_EXT_metal_surface
#endif
```

## Shader Pipeline

Shaders compiled via glslang → SPIRV, reflected via spirv-cross-core. No direct GLSL string injection.

## Debug / Validation

When `VKM_DEBUG_NAME_ENABLED` is defined, apply debug names via `vkSetDebugUtilsObjectNameEXT`. Always guard with `#ifdef VKM_DEBUG_NAME_ENABLED`.

Validation layers active in Debug builds. Do not suppress validation errors by changing flags.

## Implementation Checklist

- [ ] New Vulkan objects have debug names (when `VKM_DEBUG_NAME_ENABLED`)
- [ ] All `VkResult` values checked; log failures with `VKM_LOG_ERROR`
- [ ] Destroy order is reverse of creation order
- [ ] VMA allocations freed before VmaAllocator destruction
- [ ] No `vulkan.h` include (use volk)
- [ ] Platform surface code guarded with `#ifdef VKM_PLATFORM_WINDOWS` / `VKM_PLATFORM_APPLE`
