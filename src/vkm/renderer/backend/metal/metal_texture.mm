// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_gpu_heap_allocator.h>
#include <vkm/renderer/backend/metal/metal_util.h>
#include <Metal/MTLTexture.h>
#include <Metal/MTLDevice.h>

#include <algorithm>

namespace vkm
{
    namespace
    {
        MTLTextureUsage getMTLTextureUsage(VkmResourceCreateInfo flags)
        {
            MTLTextureUsage usage = MTLTextureUsageUnknown;
            if ((flags & VkmResourceCreateInfo::AllowShaderRead) != 0)
            {
                usage |= MTLTextureUsageShaderRead;
            }
            if ((flags & VkmResourceCreateInfo::AllowShaderWrite) != 0)
            {
                usage |= MTLTextureUsageShaderWrite;
            }
            if ((flags & VkmResourceCreateInfo::AllowColorAttachment) != 0 ||
                (flags & VkmResourceCreateInfo::AllowDepthStencilAttachment) != 0)
            {
                usage |= MTLTextureUsageRenderTarget;
            }
            return usage;
        }

        /*
        * @brief Decision policy for CPU-writable texture storage.
        * @details Shared storage lets uploadToTexture write pixels straight in via replaceRegion:,
        * skipping the staging buffer and the queue submit, but only pays off when the GPU reads
        * that same memory at no extra cost, on a unified-memory device.
        * Restricted to plain upload destinations: render targets and presentables are GPU-written
        * and never CPU-written, so putting them in Shared storage would trade attachment bandwidth
        * for a fast path they never take.
        * @param driverMetal Driver whose unified-memory answer decides availability.
        * @param info Texture being created.
        * @return True when the texture should use Shared storage.
        */
        bool shouldUseHostWritableTexture(const VkmDriverMetal* driverMetal, const VkmTextureInfo& info)
        {
            if (!driverMetal->hasUnifiedMemory())
            {
                return false;
            }
            if ((info._flags & VkmResourceCreateInfo::AllowTransferDst) == 0)
            {
                return false;
            }
            return (info._flags & VkmResourceCreateInfo::AllowColorAttachment) == 0 &&
                   (info._flags & VkmResourceCreateInfo::AllowDepthStencilAttachment) == 0 &&
                   (info._flags & VkmResourceCreateInfo::AllowPresent) == 0;
        }

        /*
        * @brief Whether this texture is created directly rather than placed in the heap pool.
        * @details Mirrors vulkan_texture.cpp's shouldUseDedicatedTexture: an explicit hint
        * wins, and Auto commits only attachments. An explicit Heap on an attachment is
        * honoured -- MTLTextureUsageRenderTarget is valid from an MTLHeapTypeAutomatic heap,
        * which does not alias unless makeAliasable is called and inherits the heap's hazard
        * tracking.
        * @param isHostWritable Whether the descriptor's storage mode is MTLStorageModeShared.
        */
        bool shouldUseCommittedTexture(const VkmTextureInfo& info, bool isHostWritable)
        {
            // The heap pool's MTLHeap is MTLStorageModePrivate and Metal requires a placed
            // resource's storage mode to match its heap's, so a Shared texture cannot go in it
            // -- the same rule shouldUseCommittedBuffer applies to a Shared buffer.
            if (isHostWritable)
            {
                if (info._placementHint == VkmMemoryPlacementHint::Heap)
                {
                    VKM_DEBUG_WARN("A host-writable texture cannot be heap-placed; texture will be committed");
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
            return (info._flags & VkmResourceCreateInfo::AllowColorAttachment) != 0 ||
                   (info._flags & VkmResourceCreateInfo::AllowDepthStencilAttachment) != 0;
        }
    }

    VkmTextureMetal::VkmTextureMetal(VkmDriverBase* driver)
        : VkmTexture(driver)
    {
    }

    VkmTextureMetal::~VkmTextureMetal()
    {
        _mtlTexture = nil; // ARC releases the Metal object
        // A placement heap reclaims nothing on its own, so the range has to go back by hand.
        // Ordered after the release above: the range must not be reusable while the object
        // placed there is still alive.
        if (_heapPlacement.isValid())
        {
            // Skipped when the allocator is already gone: destroyInner() releases every block
            // before ~VkmDriverBase destroys the resource pool, so a resource outliving it has
            // nothing left to hand the range back to -- and its ownerBlock would dangle.
            VkmGpuHeapAllocatorMetal* heapAllocator = static_cast<VkmDriverMetal*>(_driver)->getHeapAllocator();
            if (heapAllocator != nullptr)
            {
                heapAllocator->release(_heapPlacement);
            }
            _heapPlacement = VkmGpuHeapAllocatorMetal::Placement{};
        }
    }

    VkmTextureInfo VkmTextureMetal::getTextureInfoFromMTLTexture(id<MTLTexture> mtlTexture)
    {
        VkmTextureInfo info = {
            ._extent = {[mtlTexture width], [mtlTexture height], [mtlTexture depth]},
        };
        return info;
    }

    bool VkmTextureMetal::initialize(VkmResourceHandle handle, const VkmTextureInfo& info)
    {
        if (!initializeTextureCommon(handle, info))
        {
            return false;
        }

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0 &&
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) == 0)
        {
            const bool isCube = (info._type == VkmTextureType::Cube);
            VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
            _isHostWritable = shouldUseHostWritableTexture(driverMetal, info);
            // Always granted: the backend already refuses to initialize below MTLGPUFamilyApple9,
            // where memoryless is unconditional. Cannot collide with Shared storage either --
            // VkmDriverBase::newTexture strips Transient from anything carrying AllowTransferDst,
            // which shouldUseHostWritableTexture requires.
            _isTransient = (info._flags & VkmResourceCreateInfo::Transient) != 0;

            MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
            descriptor.textureType = isCube ? MTLTextureTypeCube
                                            : ((info._numArrayLayers > 1) ? MTLTextureType2DArray : MTLTextureType2D);
            descriptor.pixelFormat = getMTLPixelFormat(info._format);
            descriptor.width = info._extent.x;
            descriptor.height = info._extent.y;
            descriptor.mipmapLevelCount = info._numMipLevels;
            // MTLTextureTypeCube carries its 6 slices implicitly and counts arrayLength in
            // whole cubes -- passing 6 here would ask for a 6-cube array instead.
            descriptor.arrayLength = isCube ? 1 : info._numArrayLayers;
            descriptor.usage = getMTLTextureUsage(info._flags);
            // Shared is what makes replaceRegion: legal; on a Private texture it is invalid.
            descriptor.storageMode = _isTransient ? MTLStorageModeMemoryless
                                                  : (_isHostWritable ? MTLStorageModeShared : MTLStorageModePrivate);

            id<MTLDevice> device = driverMetal->getMTLDevice();
            // Also the "can this be heap-placed at all" test: Metal reports a zero footprint
            // for a descriptor it cannot place, and overwriting _memoryAlignment with that 0
            // would break the non-zero-alignment invariant the memory tags report. A memoryless
            // descriptor is one of those -- MTLHeap has no memoryless storage mode.
            MTLSizeAndAlign sizeAndAlign = _isTransient ? MTLSizeAndAlign{0, 0}
                                                        : [device heapTextureSizeAndAlignWithDescriptor:descriptor];
            const bool isHeapPlaceable = (sizeAndAlign.size > 0 && sizeAndAlign.align > 0);
            if (isHeapPlaceable)
            {
                _memoryAlignment = (uint32_t)sizeAndAlign.align;
            }

            if (isHeapPlaceable && !shouldUseCommittedTexture(info, _isHostWritable))
            {
                _mtlTexture = driverMetal->getHeapAllocator()->allocateTexture(
                    descriptor, sizeAndAlign.size, sizeAndAlign.align, &_heapPlacement);
            }
            if (_mtlTexture == nil)
            {
                // Committed, or the heap had no room -- the same fall-through the buffer path takes.
                _mtlTexture = [device newTextureWithDescriptor:descriptor];
            }
            [descriptor release];
            if (_mtlTexture == nil)
            {
                VKM_DEBUG_ERROR("Failed to create MTLTexture");
                return false;
            }
            // A memoryless texture has no IOAccelResource at any point in its lifetime, so this
            // is the size it occupies -- stated rather than queried, matching the Vulkan path.
            _allocatedSize = _isTransient ? 0 : [_mtlTexture allocatedSize];
        }

        return true;
    }

    bool VkmTextureMetal::writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer)
    {
        if (!_isHostWritable || _mtlTexture == nil)
        {
            VKM_DEBUG_ERROR("writeRegion requires a host-writable texture");
            return false;
        }

        const uint32_t mipWidth = std::max(1u, _textureInfo._extent.x >> mipLevel);
        const uint32_t mipHeight = std::max(1u, _textureInfo._extent.y >> mipLevel);
        // Tightly packed, matching what onCopyBufferToTexture computes for the staging path
        // so both routes agree on the source pitch.
        const uint32_t bytesPerRow = mipWidth * vkmBytesPerTexel(_textureInfo._format);
        const uint64_t expectedSize = static_cast<uint64_t>(bytesPerRow) * mipHeight;
        if (size < expectedSize)
        {
            VKM_DEBUG_ERROR("writeRegion: the source is smaller than the destination mip level");
            return false;
        }

        // Metal has no image layouts and Shared storage is CPU/GPU coherent, so unlike the
        // Vulkan path there is nothing to transition and nothing to flush.
        const MTLRegion region = MTLRegionMake2D(0, 0, mipWidth, mipHeight);
        [_mtlTexture replaceRegion:region
                       mipmapLevel:mipLevel
                             slice:arrayLayer
                         withBytes:data
                       bytesPerRow:bytesPerRow
                     bytesPerImage:0]; // 2D destination: Metal requires 0 here
        return true;
    }

    // Binding a handle here does not make it resident: the common newTexture residency hook has
    // already run and found nothing to register. A caller supplying a non-swapchain external
    // texture must register it itself via VkmRenderResourcePoolMetal::registerExternalAllocation
    // (idempotent, and paired with unregisterExternalAllocation before release). The swapchain
    // deliberately does not -- CAMetalLayer.residencySet already covers its drawables, and
    // registering them individually would retain every drawable the layer ever vends, since a
    // residency set retains its members and only the last drawable is ever released back.
    bool VkmTextureMetal::overrideExternalHandle(void* externalHandle)
    {
        _mtlTexture = static_cast<id<MTLTexture>>(externalHandle);
        // TODO(snowapril) : validate external handle with current texture info
        return true;
    }

    void VkmTextureMetal::setDebugName(const char* name)
    {
        [(id<MTLTexture>)_mtlTexture setLabel:[NSString stringWithUTF8String:name]];
    }
} // namespace vkm