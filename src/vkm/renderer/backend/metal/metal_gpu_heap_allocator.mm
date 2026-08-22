// Copyright (c) 2026 Snowapril

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

    VkmGpuHeapPoolMetal* VkmGpuHeapAllocatorMetal::growBlock()
    {
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

    id<MTLBuffer> VkmGpuHeapAllocatorMetal::allocateBuffer(uint64_t lengthBytes, uint64_t footprintBytes,
                                                           uint64_t alignment, uint64_t options,
                                                           Placement* outPlacement)
    {
        *outPlacement = Placement{};
        if (footprintBytes > VkmGpuHeapPoolMetal::POOL_BLOCK_SIZE_BYTES)
        {
            VKM_DEBUG_ERROR("Allocation exceeds heap block size; use a committed allocation instead");
            return nil;
        }

        for (auto& block : _blocks)
        {
            VkmGpuMemoryAllocation range{};
            id<MTLBuffer> buffer = block->tryAllocateBuffer(lengthBytes, footprintBytes, alignment, options, &range);
            if (buffer != nil)
            {
                *outPlacement = Placement{range, block.get(), footprintBytes};
                return buffer;
            }
        }

        VkmGpuHeapPoolMetal* newBlock = growBlock();
        if (newBlock == nullptr)
        {
            return nil;
        }

        VkmGpuMemoryAllocation range{};
        id<MTLBuffer> buffer = newBlock->tryAllocateBuffer(lengthBytes, footprintBytes, alignment, options, &range);
        if (buffer != nil)
        {
            *outPlacement = Placement{range, newBlock, footprintBytes};
        }
        return buffer;
    }

    id<MTLTexture> VkmGpuHeapAllocatorMetal::allocateTexture(MTLTextureDescriptor* descriptor, uint64_t footprintBytes,
                                                             uint64_t alignment, Placement* outPlacement)
    {
        *outPlacement = Placement{};
        if (footprintBytes > VkmGpuHeapPoolMetal::POOL_BLOCK_SIZE_BYTES)
        {
            VKM_DEBUG_ERROR("Allocation exceeds heap block size; use a committed allocation instead");
            return nil;
        }

        for (auto& block : _blocks)
        {
            VkmGpuMemoryAllocation range{};
            id<MTLTexture> texture = block->tryAllocateTexture(descriptor, footprintBytes, alignment, &range);
            if (texture != nil)
            {
                *outPlacement = Placement{range, block.get(), footprintBytes};
                return texture;
            }
        }

        VkmGpuHeapPoolMetal* newBlock = growBlock();
        if (newBlock == nullptr)
        {
            return nil;
        }

        VkmGpuMemoryAllocation range{};
        id<MTLTexture> texture = newBlock->tryAllocateTexture(descriptor, footprintBytes, alignment, &range);
        if (texture != nil)
        {
            *outPlacement = Placement{range, newBlock, footprintBytes};
        }
        return texture;
    }

    void VkmGpuHeapAllocatorMetal::release(const Placement& placement)
    {
        if (!placement.isValid())
        {
            return;
        }
        placement.ownerBlock->release(placement.range, placement.footprintBytes);
    }

    void VkmGpuHeapAllocatorMetal::accumulateMemoryStats(VkmGpuMemoryStats* outStats) const
    {
        if (outStats == nullptr)
        {
            return;
        }

        for (const auto& block : _blocks)
        {
            id<MTLHeap> heap = block->getHeap();
            if (heap == nil)
            {
                continue;
            }
            outStats->_poolReservedBytes += static_cast<uint64_t>([heap currentAllocatedSize]);
            // From our own bookkeeping: a placement heap does not maintain usedSize.
            outStats->_poolUsedBytes += block->getUsedBytes();
            outStats->_hasPoolStats = true;
        }
    }

    void VkmGpuHeapAllocatorMetal::destroy()
    {
        _blocks.clear();
    }
} // namespace vkm
