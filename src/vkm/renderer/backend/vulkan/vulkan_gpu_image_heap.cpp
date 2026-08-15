// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_gpu_image_heap.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <vk_mem_alloc.h>

namespace vkm
{
    VkmGpuImageHeapVulkan::VkmGpuImageHeapVulkan(VkmDriverVulkan* driver)
        : _driver(driver)
    {
    }

    VkmGpuImageHeapVulkan::~VkmGpuImageHeapVulkan()
    {
        destroy();
    }

    bool VkmGpuImageHeapVulkan::initialize(uint64_t sizeBytes, uint32_t memoryTypeBits)
    {
        // Synthesized rather than taken from a real image: the block hosts many images, so what
        // it must satisfy is their *intersection*. 64 KiB is the coarsest alignment any of the
        // optimal-tiled formats here asks for, so a block aligned to it can satisfy each image's
        // own alignment at some offset inside.
        constexpr VkDeviceSize kBlockAlignment = 64ull * 1024;
        const VkMemoryRequirements memoryRequirements{
            .size           = sizeBytes,
            .alignment      = kBlockAlignment,
            .memoryTypeBits = memoryTypeBits,
        };

        VmaAllocationCreateInfo allocCreateInfo{};
        // Not VMA_MEMORY_USAGE_AUTO: the AUTO family infers a memory type from the buffer or
        // image being created, and asserts when there is none -- which is exactly this call,
        // a bare block that many images will later be bound into. UNKNOWN plus an explicit
        // requiredFlags is the documented shape for a raw allocation.
        allocCreateInfo.usage = VMA_MEMORY_USAGE_UNKNOWN;
        // DEDICATED forces a whole VkDeviceMemory rather than a sub-range of VMA's own block --
        // required, since offsets handed to vkBindImageMemory are relative to the allocation.
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        allocCreateInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        const VkResult vkResult = vmaAllocateMemory(_driver->getVmaAllocator(), &memoryRequirements,
                                                    &allocCreateInfo, &_vmaAllocation, nullptr);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to allocate an aliasing image heap block"))
        {
            return false;
        }

        _sizeBytes = sizeBytes;
        return true;
    }

    void VkmGpuImageHeapVulkan::destroy()
    {
        if (_vmaAllocation != nullptr)
        {
            // Every image bound into this block must already be destroyed: an image outlives its
            // memory only if someone bypassed the deferred reclaimer.
            vmaFreeMemory(_driver->getVmaAllocator(), _vmaAllocation);
            _vmaAllocation = nullptr;
            _sizeBytes = 0;
        }
    }

    bool VkmGpuImageHeapVulkan::bindImage(VkImage image, uint64_t offset)
    {
        if (_vmaAllocation == nullptr)
        {
            VKM_DEBUG_ERROR("Cannot bind an image into an uninitialized aliasing heap block");
            return false;
        }

        const VkResult vkResult =
            vmaBindImageMemory2(_driver->getVmaAllocator(), _vmaAllocation, offset, image, nullptr);
        return VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to bind an aliased image into its heap block");
    }
} // namespace vkm
