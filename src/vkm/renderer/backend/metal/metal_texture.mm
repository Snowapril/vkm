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

    VkmTextureInfo VkmTextureMetal::getTextureInfoFromMTLTexture(MTLTexture* mtlTexture)
    {
        id<MTLTexture> mtlTextureCasted = static_cast<id<MTLTexture>>(mtlTexture);
        VkmTextureInfo info = {
            ._extent = {[mtlTextureCasted width], [mtlTextureCasted height], [mtlTextureCasted depth]},
        };
        return info;
    }

    bool VkmTextureMetal::initialize(const VkmTextureInfo& info, void* externalHandleOrNull)
    {
        if (!initializeCommon(info))
        {
            return false;
        }

        if (externalHandleOrNull != nullptr)
        {
            _mtlTexture = static_cast<MTLTexture*>(externalHandleOrNull);
        }
        else if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0)
        {
            // TODO(snowapril) : create texture with info
        }

        return true;
    }

    void VkmTextureMetal::setDebugName(const char* name)
    {
        [(id<MTLTexture>)_mtlTexture setLabel:[NSString stringWithUTF8String:name]];
    }
} // namespace vkm