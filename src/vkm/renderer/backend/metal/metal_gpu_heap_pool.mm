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

    id<MTLBuffer> VkmGpuHeapPoolMetal::tryAllocateBuffer(uint64_t sizeBytes, uint64_t alignment, uint64_t options)
    {
        if ([_mtlHeap maxAvailableSizeWithAlignment:(NSUInteger)alignment] < sizeBytes)
        {
            return nil;
        }
        return [_mtlHeap newBufferWithLength:(NSUInteger)sizeBytes options:(MTLResourceOptions)options];
    }
} // namespace vkm
