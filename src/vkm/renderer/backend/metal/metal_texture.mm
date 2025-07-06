// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_texture.h>
#include <Metal/MTLTexture.h>

namespace vkm
{
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

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0)
        {
            // TODO(snowapril) : create texture with info
        }

        return true;
    }

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