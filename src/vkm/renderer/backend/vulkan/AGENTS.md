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
// VmaAllocator is owned by VkmDriverVulkan, created in initializeInner(), destroyed
// last in destroyInner() (after all buffer/texture/pool teardown).
```
Texture/Buffer placement (committed vs. pooled) is decided per-resource inside each
concrete class's own `initialize()` (see `shouldUseDedicatedTexture`/`shouldUseCommittedBuffer`
in `vulkan_texture.cpp`/`vulkan_buffer.cpp`): an explicit `VkmMemoryPlacementHint` always
wins; `Auto` forces committed for large/attachment/read-write/externally-owned resources
and otherwise pools. Sampler has no memory backing at all (`vkCreateSampler` involves no
VMA/`VkDeviceMemory`). StagingBuffer is always committed + persistently host-mapped
(`VMA_ALLOCATION_CREATE_MAPPED_BIT`), never pooled.

Pooled buffers are suballocated from `VkmGpuBufferPoolVulkan` (one shared 64 MiB `VkBuffer`
+ dedicated VMA allocation per block, carved up via the vendored `VkmOffsetAllocator`
wrapper around OffsetAllocator — see `common/gpu_offset_allocator.h`). `VkmDriverVulkan`
owns the growable list of pool blocks and creates a new one on exhaustion. Pooled textures
are placed by VMA's own internal suballocator (plain `vmaCreateImage` without the dedicated
bit) — no custom allocator involved for textures, since VMA already does this.

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
- `initializeInner` — VkInstance, VkPhysicalDevice, VkDevice creation, VmaAllocator creation
- `destroyInner` — cleanup in reverse order, VmaAllocator destruction last
- `newTextureInner` — returns `new VkmTextureVulkan`
- `newBufferInner` — returns `new VkmBufferVulkan`
- `newStagingBufferInner` — returns `new VkmStagingBufferVulkan`
- `newSamplerInner` — returns `new VkmSamplerVulkan`
- `newTextureViewInner` — returns `new VkmTextureViewVulkan`
- `newBufferViewInner` — returns `new VkmBufferViewVulkan`
- `newSwapChainInner` — returns `new VkmSwapChainVulkan`
- `newCommandQueueInner` → returns `new VkmCommandQueueVulkan(this)` (see `vulkan_command_queue.h`)

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

## Coordinate Space (Y Flip)

The engine's clip space is +Y up (see `include/vkm/renderer/backend/common/AGENTS.md`), but
Vulkan's NDC is +Y down. Vulkan is the only backend that has to compensate, and it does so in
two places that must stay in sync:

1. `src/tools/vkm-compiler/main.cpp` — `compileToSpirv` adds DXC's **`-fvk-invert-y`** for the
   Vulkan target only, negating `SV_Position.y` so +Y-up clip space lands right-side-up on
   Vulkan's +Y-down NDC. This is the single Y-flip site; the command buffer uses a plain
   positive-height viewport.
2. `vulkan_pipeline_state.cpp` — `toVkFrontFace` maps `CounterClockwise` to
   `VK_FRONT_FACE_CLOCKWISE` and vice versa, cancelling the winding mirror the Y-flip
   introduces.

The inverted enum mapping looks like a bug in isolation. Neither change is correct without the
other — do not touch one alone. (The winding flip is intrinsic to the +Y-up convention: any
vertical flip reverses framebuffer winding, no matter which layer performs it, so moving the
Y-flip does not remove the front-face inversion.)

Changing `-fvk-invert-y` requires regenerating the `.vfcache` shader caches (they are
`vkm-compiler` output); a stale cache silently masks the change.

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
