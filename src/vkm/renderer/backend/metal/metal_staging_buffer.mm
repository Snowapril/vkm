// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_staging_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#import <Metal/Metal.h>

#include <cstring>

namespace vkm
{
    VkmStagingBufferMetal::VkmStagingBufferMetal(VkmDriverBase* driver)
        : VkmStagingBuffer(driver)
    {
    }

    VkmStagingBufferMetal::~VkmStagingBufferMetal()
    {
        _mtlBuffer = nil; // ARC releases
    }

    bool VkmStagingBufferMetal::initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info)
    {
        if (!initializeStagingBufferCommon(handle, info))
        {
            return false;
        }

        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
        id<MTLDevice> device = driverMetal->getMTLDevice();

        // Always committed + host-visible; never pooled.
        _mtlBuffer = [device newBufferWithLength:info._size options:MTLResourceStorageModeShared];
        if (_mtlBuffer == nil)
        {
            VKM_DEBUG_ERROR("Failed to create MTLBuffer for staging buffer");
            return false;
        }

        _mappedPointer = [_mtlBuffer contents];
        return true;
    }

    void* VkmStagingBufferMetal::map()
    {
        return _mappedPointer;
    }

    void VkmStagingBufferMetal::unmap()
    {
        // No-op: a Shared-mode buffer's contents pointer is valid for its whole lifetime --
        // there is no explicit map/unmap step in the Metal API at all.
    }

    void VkmStagingBufferMetal::flush(uint64_t, uint64_t)
    {
        // No-op: MTLStorageModeShared is coherent between CPU and GPU.
    }

    void VkmStagingBufferMetal::writeDirect(uint64_t offset, const void* data, uint64_t size)
    {
        std::memcpy(static_cast<uint8_t*>(_mappedPointer) + offset, data, size);
    }

    void VkmStagingBufferMetal::setDebugName(const char* name)
    {
        [_mtlBuffer setLabel:[NSString stringWithUTF8String:name]];
    }
} // namespace vkm
