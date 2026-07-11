// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <volk.h>
#include <memory>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace vkm
{
    class VkmDriverVulkan;

    /*
    * @brief One shared VkBuffer + VMA-dedicated backing allocation, carved into
    * sub-ranges via VkmOffsetAllocator. Used for the pooled placement path of
    * regular (non-staging) buffers; StagingBuffer is always committed instead.
    */
    class VkmGpuBufferPoolVulkan
    {
    public:
        static constexpr const VkDeviceSize POOL_BLOCK_SIZE_BYTES = 64ull * 1024 * 1024;

        explicit VkmGpuBufferPoolVulkan(VkmDriverVulkan* driver);
        ~VkmGpuBufferPoolVulkan();

        bool initialize();

        bool tryAllocate(uint64_t sizeBytes, uint32_t alignment, VkmGpuMemoryAllocation* outAllocation);
        void release(const VkmGpuMemoryAllocation& allocation);

        inline VkBuffer getBuffer() const { return _vkBuffer; }

    private:
        VkmDriverVulkan* _driver;
        VkBuffer _vkBuffer{VK_NULL_HANDLE};
        VmaAllocation _vmaAllocation{nullptr};
        std::unique_ptr<VkmOffsetAllocator> _offsetAllocator;
    };
} // namespace vkm
