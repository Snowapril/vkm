// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/texture.h>

namespace vkm
{
    VkmTexture::VkmTexture(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmTexture::~VkmTexture()
    {
    }

    bool VkmTexture::initializeTextureCommon(VkmResourceHandle handle, const VkmTextureInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _textureInfo = info;
        return true;
    }
} // namespace vkm