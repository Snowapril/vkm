// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_gpu_heap_pool.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#import <Metal/Metal.h>

namespace vkm
{
    VkmGpuHeapPoolMetal::VkmGpuHeapPoolMetal(VkmDriverMetal* driver)
        : _driver(driver)
    {
    }

    VkmGpuHeapPoolMetal::~VkmGpuHeapPoolMetal()
    {
        _mtlHeap = nil; // ARC releases; any still-live sub-allocated buffers keep it alive
    }

    bool VkmGpuHeapPoolMetal::initialize()
    {
        MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.size = POOL_BLOCK_SIZE_BYTES;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.type = MTLHeapTypeAutomatic;

        id<MTLDevice> device = _driver->getMTLDevice();
        _mtlHeap = [device newHeapWithDescriptor:descriptor];
        return _mtlHeap != nil;
    }

    bool VkmGpuHeapPoolMetal::hasSpaceFor(uint64_t sizeBytes, uint64_t alignment) const
    {
        return [_mtlHeap maxAvailableSizeWithAlignment:(NSUInteger)alignment] >= sizeBytes;
    }

    id<MTLBuffer> VkmGpuHeapPoolMetal::tryAllocateBuffer(uint64_t sizeBytes, uint64_t alignment, uint64_t options)
    {
        if (!hasSpaceFor(sizeBytes, alignment))
        {
            return nil;
        }
        return [_mtlHeap newBufferWithLength:(NSUInteger)sizeBytes options:(MTLResourceOptions)options];
    }

    id<MTLTexture> VkmGpuHeapPoolMetal::tryAllocateTexture(MTLTextureDescriptor* descriptor,
                                                          uint64_t sizeBytes, uint64_t alignment)
    {
        if (!hasSpaceFor(sizeBytes, alignment))
        {
            return nil;
        }
        // The descriptor's storageMode must already match the heap's (MTLStorageModePrivate);
        // VkmTextureMetal is what enforces that before calling here.
        return [_mtlHeap newTextureWithDescriptor:descriptor];
    }
} // namespace vkm
