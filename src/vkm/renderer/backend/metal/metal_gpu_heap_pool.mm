// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_gpu_heap_pool.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#import <Metal/Metal.h>

#include <algorithm>

namespace vkm
{
    VkmGpuHeapPoolMetal::VkmGpuHeapPoolMetal(VkmDriverMetal* driver)
        : _driver(driver)
    {
    }

    VkmGpuHeapPoolMetal::~VkmGpuHeapPoolMetal()
    {
        _mtlHeap = nil; // ARC releases; any still-live placed resources keep it alive
    }

    bool VkmGpuHeapPoolMetal::initialize()
    {
        MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.size = POOL_BLOCK_SIZE_BYTES;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.type = MTLHeapTypePlacement;
        // Requested rather than defaulted: a placement heap is untracked by default, which
        // would make every placed resource the caller's synchronization problem. Tracked keeps
        // them behaving like committed resources for the rest of the engine.
        descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;

        id<MTLDevice> device = _driver->getMTLDevice();
        _mtlHeap = [device newHeapWithDescriptor:descriptor];
        [descriptor release];
        if (_mtlHeap == nil)
        {
            VKM_DEBUG_ERROR("Failed to create MTLHeap");
            return false;
        }

        // Sized to what Metal actually handed back, which may exceed the requested size.
        _offsetAllocator = std::make_unique<VkmOffsetAllocator>(static_cast<uint32_t>([_mtlHeap size]));
        return true;
    }

    id<MTLBuffer> VkmGpuHeapPoolMetal::tryAllocateBuffer(uint64_t lengthBytes, uint64_t footprintBytes,
                                                         uint64_t alignment, uint64_t options,
                                                         VkmGpuMemoryAllocation* outAllocation)
    {
        *outAllocation = _offsetAllocator->allocate(static_cast<uint32_t>(footprintBytes), static_cast<uint32_t>(alignment));
        if (!outAllocation->isValid())
        {
            return nil;
        }

        // Reserved by footprint, created at the requested length: the heap needs the padded
        // size, but the buffer must report the length its caller asked for.
        id<MTLBuffer> buffer = [_mtlHeap newBufferWithLength:(NSUInteger)lengthBytes
                                                     options:(MTLResourceOptions)options
                                                      offset:(NSUInteger)outAllocation->_offset];
        if (buffer == nil)
        {
            _offsetAllocator->free(*outAllocation);
            *outAllocation = VkmGpuMemoryAllocation{};
            return nil;
        }

        _usedBytes += footprintBytes;
        return buffer;
    }

    id<MTLTexture> VkmGpuHeapPoolMetal::tryAllocateTexture(MTLTextureDescriptor* descriptor, uint64_t footprintBytes,
                                                           uint64_t alignment, VkmGpuMemoryAllocation* outAllocation)
    {
        *outAllocation = _offsetAllocator->allocate(static_cast<uint32_t>(footprintBytes), static_cast<uint32_t>(alignment));
        if (!outAllocation->isValid())
        {
            return nil;
        }

        // The descriptor's storageMode must already match the heap's MTLStorageModePrivate;
        // VkmTextureMetal enforces that before calling here.
        id<MTLTexture> texture = [_mtlHeap newTextureWithDescriptor:descriptor
                                                             offset:(NSUInteger)outAllocation->_offset];
        if (texture == nil)
        {
            _offsetAllocator->free(*outAllocation);
            *outAllocation = VkmGpuMemoryAllocation{};
            return nil;
        }

        _usedBytes += footprintBytes;
        return texture;
    }

    void VkmGpuHeapPoolMetal::release(const VkmGpuMemoryAllocation& allocation, uint64_t footprintBytes)
    {
        if (!allocation.isValid())
        {
            return;
        }
        _offsetAllocator->free(allocation);
        _usedBytes -= std::min(_usedBytes, footprintBytes);
    }
} // namespace vkm
