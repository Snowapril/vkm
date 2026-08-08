// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_gpu_heap_allocator.h>
#include <vkm/renderer/backend/metal/metal_gpu_heap_pool.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#import <Metal/MTLHeap.h>

namespace vkm
{
    VkmGpuHeapAllocatorMetal::VkmGpuHeapAllocatorMetal(VkmDriverMetal* driver)
        : _driver(driver)
    {
    }

    VkmGpuHeapAllocatorMetal::~VkmGpuHeapAllocatorMetal()
    {
        destroy();
    }

    VkmGpuHeapPoolMetal* VkmGpuHeapAllocatorMetal::acquireBlockWithSpace(uint64_t sizeBytes, uint64_t alignment)
    {
        for (auto& block : _blocks)
        {
            if (block->hasSpaceFor(sizeBytes, alignment))
            {
                return block.get();
            }
        }

        if (sizeBytes > VkmGpuHeapPoolMetal::POOL_BLOCK_SIZE_BYTES)
        {
            VKM_DEBUG_ERROR("Allocation exceeds heap block size; use a committed allocation instead");
            return nullptr;
        }

        auto newBlock = std::make_unique<VkmGpuHeapPoolMetal>(_driver);
        if (!newBlock->initialize())
        {
            return nullptr;
        }

        // Placed sub-allocations do not make the backing heap resident on their own; register
        // the whole block so every resource placed in it is covered.
        VkmRenderResourcePoolMetal* renderResourcePoolMetal =
            static_cast<VkmRenderResourcePoolMetal*>(_driver->getRenderResourcePool());
        renderResourcePoolMetal->registerExternalAllocation(newBlock->getHeap());

        _blocks.push_back(std::move(newBlock));
        return _blocks.back().get();
    }

    id<MTLBuffer> VkmGpuHeapAllocatorMetal::allocateBuffer(uint64_t sizeBytes, uint64_t alignment, uint64_t options)
    {
        VkmGpuHeapPoolMetal* block = acquireBlockWithSpace(sizeBytes, alignment);
        return (block != nullptr) ? block->tryAllocateBuffer(sizeBytes, alignment, options) : nil;
    }

    id<MTLTexture> VkmGpuHeapAllocatorMetal::allocateTexture(MTLTextureDescriptor* descriptor,
                                                             uint64_t sizeBytes, uint64_t alignment)
    {
        VkmGpuHeapPoolMetal* block = acquireBlockWithSpace(sizeBytes, alignment);
        return (block != nullptr) ? block->tryAllocateTexture(descriptor, sizeBytes, alignment) : nil;
    }

    void VkmGpuHeapAllocatorMetal::accumulateMemoryStats(VkmGpuMemoryStats* outStats) const
    {
        if (outStats == nullptr)
        {
            return;
        }

        // currentAllocatedSize is what each block reserved from the device, usedSize what has
        // been placed inside it.
        for (const auto& block : _blocks)
        {
            id<MTLHeap> heap = block->getHeap();
            if (heap == nil)
            {
                continue;
            }
            outStats->_poolReservedBytes += static_cast<uint64_t>([heap currentAllocatedSize]);
            outStats->_poolUsedBytes += static_cast<uint64_t>([heap usedSize]);
            outStats->_hasPoolStats = true;
        }
    }

    void VkmGpuHeapAllocatorMetal::destroy()
    {
        _blocks.clear();
    }
} // namespace vkm
