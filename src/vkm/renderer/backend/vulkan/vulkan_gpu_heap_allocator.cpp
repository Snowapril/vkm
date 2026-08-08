// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_gpu_heap_allocator.h>
#include <vkm/renderer/backend/vulkan/vulkan_gpu_buffer_pool.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

namespace vkm
{
    VkmGpuHeapAllocatorVulkan::VkmGpuHeapAllocatorVulkan(VkmDriverVulkan* driver)
        : _driver(driver)
    {
    }

    VkmGpuHeapAllocatorVulkan::~VkmGpuHeapAllocatorVulkan()
    {
        destroy();
    }

    bool VkmGpuHeapAllocatorVulkan::allocate(uint64_t sizeBytes, uint32_t alignment, Allocation* outAllocation)
    {
        if (outAllocation == nullptr)
        {
            return false;
        }

        for (auto& block : _blocks)
        {
            VkmGpuMemoryAllocation range{};
            if (block->tryAllocate(sizeBytes, alignment, &range))
            {
                outAllocation->buffer = block->getBuffer();
                outAllocation->range = range;
                outAllocation->ownerBlock = block.get();
                return true;
            }
        }

        if (sizeBytes > VkmGpuBufferPoolVulkan::POOL_BLOCK_SIZE_BYTES)
        {
            VKM_DEBUG_ERROR("Buffer allocation exceeds heap block size; use a committed allocation instead");
            return false;
        }

        auto newBlock = std::make_unique<VkmGpuBufferPoolVulkan>(_driver);
        if (!newBlock->initialize())
        {
            return false;
        }

        VkmGpuMemoryAllocation range{};
        if (!newBlock->tryAllocate(sizeBytes, alignment, &range))
        {
            return false;
        }

        outAllocation->buffer = newBlock->getBuffer();
        outAllocation->range = range;
        outAllocation->ownerBlock = newBlock.get();
        _blocks.push_back(std::move(newBlock));
        return true;
    }

    void VkmGpuHeapAllocatorVulkan::destroy()
    {
        _blocks.clear();
    }
} // namespace vkm
