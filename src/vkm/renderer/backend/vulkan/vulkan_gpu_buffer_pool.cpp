// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_gpu_buffer_pool.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <vk_mem_alloc.h>

namespace vkm
{
    VkmGpuBufferPoolVulkan::VkmGpuBufferPoolVulkan(VkmDriverVulkan* driver)
        : _driver(driver)
    {
    }

    VkmGpuBufferPoolVulkan::~VkmGpuBufferPoolVulkan()
    {
        if (_vmaAllocation != nullptr)
        {
            vmaDestroyBuffer(_driver->getVmaAllocator(), _vkBuffer, _vmaAllocation);
            _vkBuffer = VK_NULL_HANDLE;
            _vmaAllocation = nullptr;
        }
    }

    bool VkmGpuBufferPoolVulkan::initialize()
    {
        // Superset of usages any pooled (non-staging) buffer might request; VkBuffer usage
        // flags are fixed at creation, so the shared block must cover every combination
        // toVkBufferUsageFlags() can currently produce.
        constexpr VkBufferUsageFlags kPoolBufferUsage =
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        const VkBufferCreateInfo bufferCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = POOL_BLOCK_SIZE_BYTES,
            .usage       = kPoolBufferUsage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        const VkResult vkResult = vmaCreateBuffer(_driver->getVmaAllocator(), &bufferCreateInfo, &allocCreateInfo, &_vkBuffer, &_vmaAllocation, nullptr);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create buffer pool block via VMA"))
        {
            return false;
        }

        _offsetAllocator = std::make_unique<VkmOffsetAllocator>(static_cast<uint32_t>(POOL_BLOCK_SIZE_BYTES));
        return true;
    }

    bool VkmGpuBufferPoolVulkan::tryAllocate(uint64_t sizeBytes, uint32_t alignment, VkmGpuMemoryAllocation* outAllocation)
    {
        const VkmGpuMemoryAllocation allocation = _offsetAllocator->allocate(static_cast<uint32_t>(sizeBytes), alignment);
        if (!allocation.isValid())
        {
            return false;
        }
        *outAllocation = allocation;
        return true;
    }

    void VkmGpuBufferPoolVulkan::release(const VkmGpuMemoryAllocation& allocation)
    {
        _offsetAllocator->free(allocation);
    }
} // namespace vkm
