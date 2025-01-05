// Copyright (c) 2024 Snowapril

#include "vkm/renderer/backend/common/texture.h"

namespace vkm
{
    VkmTexture::VkmTexture(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmTexture::~VkmTexture()
    {
    }
} // namespace vkm