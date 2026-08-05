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
            // The pool's shared block is device-local, so a host-writable buffer cannot be
            // suballocated from it -- host-writable always means committed, whatever the hint says.
            if (info._accessHint == VkmMemoryAccessHint::HostWrite)
            {
                if (info._placementHint == VkmMemoryPlacementHint::ForcePooled)
                {
                    VKM_DEBUG_WARN("VkmMemoryAccessHint::HostWrite cannot be pooled; buffer will be committed");
                }
                return true;
            }
            if (info._placementHint == VkmMemoryPlacementHint::ForceCommitted)
            {
                return true;
            }
            // A build input must be committed. The pooled path returns before
            // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT is added below and inherits the pool
            // block's usage instead, so a pooled build input would fail the build with a
            // validation error naming the *block*, nowhere near the buffer that asked for it.
            if ((info._flags & VkmResourceCreateInfo::AllowAccelerationStructureInput) != 0)
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
                _allocatedSize = info._size; // no distinct VMA allocation to introspect for a pool sub-range
                _alignment = alignment;
                return true;
            }
            // Fall through to the committed path if pooling failed (e.g. size exceeds a
            // single pool block).
        }

        VkBufferUsageFlags usage = toVkBufferUsageFlags(info._flags);
        if (driverVulkan->isBufferDeviceAddressEnabled())
        {
            // Not derived from any VkmResourceCreateInfo bit: an address is a property of the
            // allocation, and every buffer is allowed to report one.
            usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }

        const VkBufferCreateInfo bufferCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = info._size,
            .usage       = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        const bool requestHostWrite = (info._accessHint == VkmMemoryAccessHint::HostWrite);
        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (requestHostWrite)
        {
            // Same allocation shape staging buffers use, minus the readback case: the CPU only
            // ever writes these, so write-combined memory is the right preference.
            allocCreateInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        }

        VmaAllocationInfo vmaAllocationInfo{};
        const VkResult vkResult = vmaCreateBuffer(driverVulkan->getVmaAllocator(), &bufferCreateInfo, &allocCreateInfo, &_vkBuffer, &_vmaAllocation, &vmaAllocationInfo);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create buffer via VMA"))
        {
            return false;
        }

        if (requestHostWrite)
        {
            // A request is not a guarantee -- report what the allocation actually landed in,
            // the same way VkmTextureVulkan re-checks its own memory properties.
            VkMemoryPropertyFlags memoryProperties = 0;
            vmaGetAllocationMemoryProperties(driverVulkan->getVmaAllocator(), _vmaAllocation, &memoryProperties);
            _isHostWritable = (memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && vmaAllocationInfo.pMappedData != nullptr;
            _mappedPointer = _isHostWritable ? vmaAllocationInfo.pMappedData : nullptr;
            if (!_isHostWritable)
            {
                VKM_DEBUG_WARN("VkmMemoryAccessHint::HostWrite was requested but the allocation is not host-visible; uploads will use the staging path");
            }
        }

        _allocatedSize = vmaAllocationInfo.size;
        VkMemoryRequirements memReqs{};
        vkGetBufferMemoryRequirements(driverVulkan->getDevice(), _vkBuffer, &memReqs);
        _alignment = (uint32_t)memReqs.alignment;
        return true;
    }

    bool VkmBufferVulkan::overrideExternalHandle(void* externalHandle)
    {
        _vkBuffer = static_cast<VkBuffer>(externalHandle);
        return true;
    }

    void* VkmBufferVulkan::map()
    {
        // VMA keeps VMA_ALLOCATION_CREATE_MAPPED_BIT allocations mapped until they are
        // destroyed, so this is an accessor; a device-local buffer has no pointer at all.
        return _mappedPointer;
    }

    void VkmBufferVulkan::unmap()
    {
        if (_vmaAllocation == nullptr)
        {
            return;
        }
        // The mapping outlives this call; flushing is the whole point, since the memory VMA
        // picked for a SEQUENTIAL_WRITE allocation is not guaranteed to be coherent.
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        vmaFlushAllocation(driverVulkan->getVmaAllocator(), _vmaAllocation, 0, VK_WHOLE_SIZE);
    }

    uint64_t VkmBufferVulkan::getGPUVirtualAddress() const
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        if (_vkBuffer == VK_NULL_HANDLE || !driverVulkan->isBufferDeviceAddressEnabled())
        {
            return 0;
        }
        const VkBufferDeviceAddressInfo addressInfo{
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = _vkBuffer,
        };
        // A pooled buffer shares the pool block's VkBuffer, so its own address is the block's
        // plus its sub-allocation offset.
        return vkGetBufferDeviceAddress(driverVulkan->getDevice(), &addressInfo) + getBufferOffset();
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
