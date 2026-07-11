// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_gpu_buffer_pool.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <vk_mem_alloc.h>

#include <algorithm>

namespace vkm
{
    namespace
    {
        // Decision policy: explicit hint always wins; Auto falls back to a size/usage
        // heuristic. Pooling is skipped for anything large, read-write, or externally owned
        // -- those are few, long-lived, and gain nothing from suballocation.
        constexpr uint64_t POOLING_SIZE_THRESHOLD_BYTES = 4ull * 1024 * 1024;

        bool shouldUseCommittedBuffer(const VkmBufferInfo& info)
        {
            if (info._placementHint == VkmMemoryPlacementHint::ForceCommitted)
            {
                return true;
            }
            if (info._placementHint == VkmMemoryPlacementHint::ForcePooled)
            {
                return false;
            }

            if (info._size >= POOLING_SIZE_THRESHOLD_BYTES)
            {
                return true;
            }
            constexpr uint32_t kShaderReadWrite = (uint32_t)VkmResourceCreateInfo::AllowShaderReadWrite;
            if ((info._flags & VkmResourceCreateInfo::AllowShaderReadWrite) == kShaderReadWrite)
            {
                return true;
            }
            if ((info._flags & VkmResourceCreateInfo::ExternalHandleOwner) != 0)
            {
                return true;
            }
            return false;
        }
    }

    VkmBufferVulkan::VkmBufferVulkan(VkmDriverBase* driver)
        : VkmBuffer(driver)
    {
    }

    VkmBufferVulkan::~VkmBufferVulkan()
    {
        if (_ownerPool != nullptr)
        {
            _ownerPool->release(_poolAllocation);
            _ownerPool = nullptr;
        }
        else if (_vmaAllocation != nullptr)
        {
            VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
            vmaDestroyBuffer(driverVulkan->getVmaAllocator(), _vkBuffer, _vmaAllocation);
            _vkBuffer = VK_NULL_HANDLE;
            _vmaAllocation = nullptr;
        }
    }

    bool VkmBufferVulkan::initialize(VkmResourceHandle handle, const VkmBufferInfo& info)
    {
        if (!initializeBufferCommon(handle, info))
        {
            return false;
        }

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) != 0 ||
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) != 0)
        {
            return true;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        if (!shouldUseCommittedBuffer(info))
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(driverVulkan->getPhysicalDevice(), &properties);

            uint32_t alignment = 16; // sane minimum default
            if ((info._flags & VkmResourceCreateInfo::AllowShaderRead) != 0)
            {
                alignment = std::max(alignment, (uint32_t)properties.limits.minUniformBufferOffsetAlignment);
            }
            if ((info._flags & VkmResourceCreateInfo::AllowShaderWrite) != 0)
            {
                alignment = std::max(alignment, (uint32_t)properties.limits.minStorageBufferOffsetAlignment);
            }

            VkmDriverVulkan::PooledBufferAllocation poolResult{};
            if (driverVulkan->allocateFromBufferPool(info._size, alignment, &poolResult))
            {
                _vkBuffer = poolResult.buffer;
                _poolAllocation = poolResult.allocation;
                _ownerPool = poolResult.ownerPool;
                return true;
            }
            // Fall through to the committed path if pooling failed (e.g. size exceeds a
            // single pool block).
        }

        const VkBufferCreateInfo bufferCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = info._size,
            .usage       = toVkBufferUsageFlags(info._flags),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        const VkResult vkResult = vmaCreateBuffer(driverVulkan->getVmaAllocator(), &bufferCreateInfo, &allocCreateInfo, &_vkBuffer, &_vmaAllocation, nullptr);
        return VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create buffer via VMA");
    }

    bool VkmBufferVulkan::overrideExternalHandle(void* externalHandle)
    {
        _vkBuffer = static_cast<VkBuffer>(externalHandle);
        return true;
    }

    void VkmBufferVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        if (_ownerPool != nullptr)
        {
            // _vkBuffer is a shared pool block here, not owned solely by this instance --
            // naming it after one sub-allocation would be misleading.
            return;
        }
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
