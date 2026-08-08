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
            // The heap pool's MTLHeap is MTLStorageModePrivate, so a Shared buffer cannot be
            // placed in it -- host-writable always means committed, whatever the hint says.
            if (info._accessHint == VkmMemoryAccessHint::HostWrite)
            {
                if (info._placementHint == VkmMemoryPlacementHint::Heap)
                {
                    VKM_DEBUG_WARN("VkmMemoryAccessHint::HostWrite cannot be heap-placed; buffer will be committed");
                }
                return true;
            }
            if (info._placementHint == VkmMemoryPlacementHint::Committed)
            {
                return true;
            }
            if (info._placementHint == VkmMemoryPlacementHint::Heap)
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
        const MTLResourceOptions storageMode = (info._accessHint == VkmMemoryAccessHint::HostWrite)
            ? MTLResourceStorageModeShared
            : MTLResourceStorageModePrivate;
        MTLSizeAndAlign sizeAndAlign = [device heapBufferSizeAndAlignWithLength:info._size options:storageMode];
        _memoryAlignment = (uint32_t)sizeAndAlign.align;

        if (!shouldUseCommittedBuffer(info))
        {
            // Pass the device-queried alignment (not a hardcoded constant) as the capacity hint
            // for the heap's maxAvailableSizeWithAlignment: check; the heap sub-allocates and
            // aligns placed buffers internally, so this only affects the capacity estimate.
            _mtlBuffer = driverMetal->allocateFromHeapPool(info._size, sizeAndAlign.align, MTLResourceStorageModePrivate);
            if (_mtlBuffer != nil)
            {
                _allocatedSize = [_mtlBuffer allocatedSize];
                return true;
            }
            // Fall through to the committed path if pooling failed (e.g. size exceeds a
            // single pool block).
        }

        _mtlBuffer = [device newBufferWithLength:info._size options:storageMode];
        if (_mtlBuffer == nil)
        {
            VKM_DEBUG_ERROR("Failed to create MTLBuffer");
            return false;
        }
        _allocatedSize = [_mtlBuffer allocatedSize];
        // Reports what was actually allocated: only the committed Shared path is host-writable,
        // and a HostWrite request always lands there.
        _isHostWritable = (storageMode == MTLResourceStorageModeShared);
        return true;
    }

    void* VkmBufferMetal::map()
    {
        // A Shared-mode buffer's contents pointer is valid for its whole lifetime; a Private
        // one has none, which is exactly what isHostWritable() reports.
        return _isHostWritable ? [_mtlBuffer contents] : nullptr;
    }

    void VkmBufferMetal::unmap()
    {
        // No-op: MTLStorageModeShared is coherent between CPU and GPU, and there is no explicit
        // map/unmap step in the Metal API at all.
    }

    uint64_t VkmBufferMetal::getGPUVirtualAddress() const
    {
        return _mtlBuffer != nil ? (uint64_t)[_mtlBuffer gpuAddress] : 0;
    }

    // Binding a handle here does not make it resident: the common newBuffer residency hook has
    // already run and found nothing to register. A caller supplying an external buffer must
    // register it itself via VkmRenderResourcePoolMetal::registerExternalAllocation (idempotent,
    // and paired with unregisterExternalAllocation before release) -- the same contract
    // VkmTextureMetal::overrideExternalHandle documents.
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
