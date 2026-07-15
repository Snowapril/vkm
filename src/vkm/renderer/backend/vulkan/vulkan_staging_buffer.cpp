// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_staging_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <vk_mem_alloc.h>
#include <cstring>

namespace vkm
{
    VkmStagingBufferVulkan::VkmStagingBufferVulkan(VkmDriverBase* driver)
        : VkmStagingBuffer(driver)
    {
    }

    VkmStagingBufferVulkan::~VkmStagingBufferVulkan()
    {
        if (_vmaAllocation != nullptr)
        {
            VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
            vmaDestroyBuffer(driverVulkan->getVmaAllocator(), _vkBuffer, _vmaAllocation);
            _vkBuffer = VK_NULL_HANDLE;
            _vmaAllocation = nullptr;
        }
    }

    bool VkmStagingBufferVulkan::initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info)
    {
        if (!initializeStagingBufferCommon(handle, info))
        {
            return false;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if ((info._flags & VkmResourceCreateInfo::AllowTransferDst) != 0)
        {
            usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        }

        const VkBufferCreateInfo bufferCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = info._size,
            .usage       = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        // Staging buffers are always committed + persistently host-mapped (never suballocated).
        // AllowTransferDst marks a readback buffer: the CPU reads it after GPU writes, so it
        // needs HOST_ACCESS_RANDOM (readable) memory rather than the write-combined memory
        // SEQUENTIAL_WRITE may select.
        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (((info._flags & VkmResourceCreateInfo::AllowTransferDst) != 0)
                ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

        VmaAllocationInfo vmaAllocationInfo{};
        const VkResult vkResult = vmaCreateBuffer(driverVulkan->getVmaAllocator(), &bufferCreateInfo, &allocCreateInfo, &_vkBuffer, &_vmaAllocation, &vmaAllocationInfo);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create staging buffer via VMA"))
        {
            return false;
        }

        _mappedPointer = vmaAllocationInfo.pMappedData;
        _allocatedSize = vmaAllocationInfo.size;
        VkMemoryRequirements memReqs{};
        vkGetBufferMemoryRequirements(driverVulkan->getDevice(), _vkBuffer, &memReqs);
        _alignment = (uint32_t)memReqs.alignment;
        return true;
    }

    void* VkmStagingBufferVulkan::map()
    {
        return _mappedPointer;
    }

    void VkmStagingBufferVulkan::unmap()
    {
        // No-op: VMA keeps VMA_ALLOCATION_CREATE_MAPPED_BIT allocations persistently mapped
        // until the allocation is destroyed.
    }

    void VkmStagingBufferVulkan::flush(uint64_t offset, uint64_t size)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        vmaFlushAllocation(driverVulkan->getVmaAllocator(), _vmaAllocation, offset, size);
    }

    void VkmStagingBufferVulkan::invalidate(uint64_t offset, uint64_t size)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        vmaInvalidateAllocation(driverVulkan->getVmaAllocator(), _vmaAllocation, offset, size);
    }

    void VkmStagingBufferVulkan::writeDirect(uint64_t offset, const void* data, uint64_t size)
    {
        std::memcpy(static_cast<uint8_t*>(_mappedPointer) + offset, data, size);
        flush(offset, size);
    }

    void VkmStagingBufferVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_BUFFER,
            .objectHandle = reinterpret_cast<uint64_t>(_vkBuffer),
            .pObjectName  = name,
        };
        vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo);
#else
        (void)name;
#endif
    }
} // namespace vkm
