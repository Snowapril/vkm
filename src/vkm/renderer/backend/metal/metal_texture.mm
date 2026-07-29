// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
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
        * Decision policy for CPU-writable texture storage. Shared storage lets
        * uploadToTexture write pixels straight in via replaceRegion:, skipping the staging
        * buffer and the queue submit entirely -- but only pays off when the GPU reads that
        * same memory at no extra cost, i.e. on a unified-memory device.
        *
        * Restricted to plain upload destinations: render targets and presentables are
        * GPU-written and never CPU-written, so putting them in Shared storage would trade
        * attachment bandwidth for a fast path they never take.
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
    }

    VkmTextureMetal::VkmTextureMetal(VkmDriverBase* driver)
        : VkmTexture(driver)
    {
    }

    VkmTextureMetal::~VkmTextureMetal()
    {
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
            descriptor.storageMode = _isHostWritable ? MTLStorageModeShared : MTLStorageModePrivate;

            id<MTLDevice> device = driverMetal->getMTLDevice();
            MTLSizeAndAlign sizeAndAlign = [device heapTextureSizeAndAlignWithDescriptor:descriptor];
            _memoryAlignment = (uint32_t)sizeAndAlign.align;

            _mtlTexture = [device newTextureWithDescriptor:descriptor];
            [descriptor release];
            if (_mtlTexture == nil)
            {
                VKM_DEBUG_ERROR("Failed to create MTLTexture");
                return false;
            }
            _allocatedSize = [_mtlTexture allocatedSize];
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