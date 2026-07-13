// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_util.h>
#include <Metal/MTLTexture.h>
#include <Metal/MTLDevice.h>

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
            MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
            descriptor.textureType = (info._numArrayLayers > 1) ? MTLTextureType2DArray : MTLTextureType2D;
            descriptor.pixelFormat = getMTLPixelFormat(info._format);
            descriptor.width = info._extent.x;
            descriptor.height = info._extent.y;
            descriptor.mipmapLevelCount = info._numMipLevels;
            descriptor.arrayLength = info._numArrayLayers;
            descriptor.usage = getMTLTextureUsage(info._flags);
            descriptor.storageMode = MTLStorageModePrivate;

            id<MTLDevice> device = static_cast<VkmDriverMetal*>(_driver)->getMTLDevice();
            _mtlTexture = [device newTextureWithDescriptor:descriptor];
            if (_mtlTexture == nil)
            {
                VKM_DEBUG_ERROR("Failed to create MTLTexture");
                return false;
            }
        }

        return true;
    }

    // A handle bound here after creation is never registered into a residency set (the swapchain, today's only such caller, bypasses the common newBuffer/newTexture residency hook entirely).
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