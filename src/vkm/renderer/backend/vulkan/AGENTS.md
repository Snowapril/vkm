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
Texture/Buffer placement is decided per-resource inside each concrete class's own
`initialize()` (see `shouldUseDedicatedTexture`/`shouldUseCommittedBuffer` in
`vulkan_texture.cpp`/`vulkan_buffer.cpp`): an explicit `VkmMemoryPlacementHint` always wins.

**`Auto` deliberately decides as little as possible, because VMA already decides it better.**
For every resource allocated *without* `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`, VMA
queries `VkMemoryDedicatedRequirements` itself (enabled by the allocator's
`vulkanApiVersion = VK_API_VERSION_1_3`) and honours both `requiresDedicatedAllocation` and
`prefersDedicatedAllocation`, then layers its own block-occupancy heuristic and a
`maxMemoryAllocationCount` exhaustion guard on top. Forcing the bit *overrides* that
driver-informed answer, so `Auto` sets it nowhere. The only thing `Auto` still forces is
attachments → dedicated, which is a statement about how the texture is used rather than about
the image, and is therefore the one call the allocator cannot make for itself.

A `VkmMemoryAccessHint::HostWrite` buffer overrides even `Heap` (with a warning) — the pool
block is device-local, so it is allocated committed with
`VMA_ALLOCATION_CREATE_MAPPED_BIT | HOST_ACCESS_SEQUENTIAL_WRITE_BIT` and re-checked with
`vmaGetAllocationMemoryProperties` before reporting `isHostWritable()`. Sampler has no memory
backing at all (`vkCreateSampler` involves no VMA/`VkDeviceMemory`). StagingBuffer is always
committed + persistently host-mapped (`VMA_ALLOCATION_CREATE_MAPPED_BIT`), never suballocated.

A `VkmResourceCreateInfo::Transient` texture adds `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` (legal
only alongside attachment usage, which `VkmDriverBase` has already guaranteed) and asks VMA for
`VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` through `preferredFlags`, **not** `requiredFlags`: a
device offering no such memory type would make `requiredFlags` fail `vmaCreateImage` outright,
while an ordinary device-local attachment is still correct — just not lazy. `shouldUseDedicatedTexture`
already returns true for every attachment, which matters here, since suballocating into a shared VMA
block would defeat the lazy commitment. The grant is read back with `vmaGetAllocationMemoryProperties`
into `isTransient()`, and a granted allocation reports `getAllocatedSize() == 0` (VMA's size is the
*virtual* one; lazily-allocated memory normally commits no pages). There is deliberately no device
capability probe — nothing needs the answer before allocating — but one modelled on
`hasUnifiedMemory()` would be the natural follow-up if a caller ever has to choose an algorithm up front.

Every buffer (pool block included) carries `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` and the
allocator `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` when
`VkmDriverVulkan::isBufferDeviceAddressEnabled()`; without that feature nothing carries either and
`getGPUVirtualAddress()` reports 0.

`Heap` buffers are suballocated from `VkmGpuBufferPoolVulkan` (one shared 64 MiB `VkBuffer`
+ dedicated VMA allocation per block, carved up via the vendored `VkmOffsetAllocator`
wrapper around OffsetAllocator — see `common/gpu_offset_allocator.h`). `VkmDriverVulkan`
owns `VkmGpuHeapAllocatorVulkan`, which owns the growable list of blocks and creates a new
one on exhaustion; buffers reach it through `VkmDriverVulkan::getHeapAllocator()`. `Heap` textures
are placed by VMA's own internal suballocator (plain `vmaCreateImage` without the dedicated
bit) — no custom allocator involved for textures, since VMA already does this. So `Heap`
means two different mechanisms here, and neither is an `MTLHeap`-style engine heap for images;
the caller's question is "may this share its backing allocation", not "which API object".

`kPoolBufferUsage` (`vulkan_gpu_buffer_pool.cpp`) must stay a superset of everything
`toVkBufferUsageFlags()` can produce — a `VkBuffer`'s usage is fixed at creation, so a
suballocation inheriting the block's usage fails validation naming the *block*, nowhere near
the buffer that asked for it.

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
- [ ] All `VkResult` values checked via `VKM_VK_CHECK_RESULT_MSG` / `VKM_VK_CHECK_RESULT_MSG_RETURN`
- [ ] Destroy order is reverse of creation order
- [ ] VMA allocations freed before VmaAllocator destruction
- [ ] No `vulkan.h` include (use volk)
- [ ] Platform surface code guarded with `#ifdef VKM_PLATFORM_WINDOWS` / `VKM_PLATFORM_APPLE`
