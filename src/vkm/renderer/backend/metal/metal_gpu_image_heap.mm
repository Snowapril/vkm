// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_gpu_image_heap.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

namespace vkm
{
    VkmGpuImageHeapMetal::VkmGpuImageHeapMetal(VkmDriverMetal* driver)
        : _driver(driver)
    {
    }

    VkmGpuImageHeapMetal::~VkmGpuImageHeapMetal()
    {
        destroy();
    }

    bool VkmGpuImageHeapMetal::initialize(uint64_t sizeBytes)
    {
        MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.size = (NSUInteger)sizeBytes;
        // Private for the same reason the buffer pool's heap is: a render target is GPU-written
        // and never CPU-touched, and Shared/Memoryless textures are excluded from aliasing by
        // VkmDriverBase's sanitizer anyway.
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.type = MTLHeapTypePlacement;

        id<MTLDevice> device = _driver->getMTLDevice();
        _mtlHeap = [device newHeapWithDescriptor:descriptor];
        [descriptor release];
        if (_mtlHeap == nil)
        {
            VKM_DEBUG_ERROR("Failed to create an aliasing image heap");
            return false;
        }

        _sizeBytes = sizeBytes;
        return true;
    }

    void VkmGpuImageHeapMetal::destroy()
    {
        // ARC releases the heap; any texture still placed in it keeps it alive, which is what
        // makes destruction order between the two irrelevant.
        _mtlHeap = nil;
        _sizeBytes = 0;
    }

    id<MTLTexture> VkmGpuImageHeapMetal::createTexture(MTLTextureDescriptor* descriptor, uint64_t offset)
    {
        if (_mtlHeap == nil)
        {
            VKM_DEBUG_ERROR("Cannot place a texture into an uninitialized aliasing image heap");
            return nil;
        }
        return [_mtlHeap newTextureWithDescriptor:descriptor offset:(NSUInteger)offset];
    }
} // namespace vkm
