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

    bool VkmTexture::initializeCommon(const VkmTextureInfo& info)
    {
        _textureInfo = info;
        return true;
    }
} // namespace vkm