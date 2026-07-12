// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#import <Metal/Metal.h>

namespace vkm
{
    namespace
    {
        // Decision policy mirrors the Vulkan side numerically: explicit hint always wins;
        // Auto forces committed for large/read-write/externally-owned buffers.
        constexpr uint64_t POOLING_SIZE_THRESHOLD_BYTES = 4ull * 1024 * 1024;

        bool shouldUseCommittedBuffer(const VkmBufferInfo& info)
        {
            if (info._placementHint == VkmMemoryPlacementHint::ForceCommitted)
            {
                return true;
            }
            if (info._placementHint == VkmMemoryPlacementHint::ForcePooled)
            {
                return false;
            }

            if (info._size >= POOLING_SIZE_THRESHOLD_BYTES)
            {
                return true;
            }
            constexpr uint32_t kShaderReadWrite = (uint32_t)VkmResourceCreateInfo::AllowShaderReadWrite;
            if ((info._flags & VkmResourceCreateInfo::AllowShaderReadWrite) == kShaderReadWrite)
            {
                return true;
            }
            if ((info._flags & VkmResourceCreateInfo::ExternalHandleOwner) != 0)
            {
                return true;
            }
            return false;
        }
    }

    VkmBufferMetal::VkmBufferMetal(VkmDriverBase* driver)
        : VkmBuffer(driver)
    {
    }

    VkmBufferMetal::~VkmBufferMetal()
    {
        _mtlBuffer = nil; // ARC releases; heap (if pooled) reclaims the space automatically
    }

    bool VkmBufferMetal::initialize(VkmResourceHandle handle, const VkmBufferInfo& info)
    {
        if (!initializeBufferCommon(handle, info))
        {
            return false;
        }

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) != 0 ||
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) != 0)
        {
            return true;
        }

        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
        id<MTLDevice> device = driverMetal->getMTLDevice();
        MTLSizeAndAlign sizeAndAlign = [device heapBufferSizeAndAlignWithLength:info._size options:MTLResourceStorageModePrivate];
        _memoryAlignment = (uint32_t)sizeAndAlign.align;

        if (!shouldUseCommittedBuffer(info))
        {
            _mtlBuffer = driverMetal->allocateFromHeapPool(info._size, 256, MTLResourceStorageModePrivate);
            if (_mtlBuffer != nil)
            {
                _allocatedSize = [_mtlBuffer allocatedSize];
                return true;
            }
            // Fall through to the committed path if pooling failed (e.g. size exceeds a
            // single pool block).
        }

        _mtlBuffer = [device newBufferWithLength:info._size options:MTLResourceStorageModePrivate];
        if (_mtlBuffer == nil)
        {
            VKM_DEBUG_ERROR("Failed to create MTLBuffer");
            return false;
        }
        _allocatedSize = [_mtlBuffer allocatedSize];
        return true;
    }

    // A handle bound here after creation is never registered into a residency set (the swapchain, today's only such caller, bypasses the common newBuffer/newTexture residency hook entirely).
    bool VkmBufferMetal::overrideExternalHandle(void* externalHandle)
    {
        _mtlBuffer = static_cast<id<MTLBuffer>>(externalHandle);
        return true;
    }

    void VkmBufferMetal::setDebugName(const char* name)
    {
        [_mtlBuffer setLabel:[NSString stringWithUTF8String:name]];
    }
} // namespace vkm
