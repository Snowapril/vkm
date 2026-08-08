// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <volk.h>

#include <memory>
#include <vector>

namespace vkm
{
    class VkmDriverVulkan;
    class VkmGpuBufferPoolVulkan;

    /*
    * @brief Owns the shared VkBuffer blocks backing VkmMemoryPlacementHint::Heap buffers.
    * @details The Vulkan counterpart to VkmGpuHeapAllocatorMetal, but suballocating one shared
    * VkBuffer per block rather than an MTLHeap, so an allocation carries an offset the caller
    * must apply. A request larger than one block is refused rather than served, so callers must
    * treat failure as "allocate this committed instead" rather than as an error.
    *
    * Unlike the Metal side, freeing is explicit: a sub-range must be handed back to its owning
    * block via VkmGpuBufferPoolVulkan::release(), or the block leaks that range.
    */
    class VkmGpuHeapAllocatorVulkan
    {
    public:
        struct Allocation
        {
            VkBuffer buffer{VK_NULL_HANDLE};
            VkmGpuMemoryAllocation range{};
            VkmGpuBufferPoolVulkan* ownerBlock{nullptr};
        };

        explicit VkmGpuHeapAllocatorVulkan(VkmDriverVulkan* driver);
        ~VkmGpuHeapAllocatorVulkan();

        /*
        * @brief Suballocates a sub-range, growing the block list by one block if needed.
        * @param sizeBytes Size to carve out.
        * @param alignment Required alignment of the returned offset.
        * @param outAllocation Receives the shared buffer, the sub-range and its owning block.
        * @return False when the request cannot be served; the caller falls back to committed.
        */
        bool allocate(uint64_t sizeBytes, uint32_t alignment, Allocation* outAllocation);

        /*
        * @brief Releases every block.
        * @details Must run while the driver's VmaAllocator is still alive, since each block
        * holds a VMA allocation.
        */
        void destroy();

    private:
        VkmDriverVulkan* _driver;
        std::vector<std::unique_ptr<VkmGpuBufferPoolVulkan>> _blocks;
    };
} // namespace vkm
