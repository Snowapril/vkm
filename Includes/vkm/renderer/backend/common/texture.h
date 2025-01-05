// Copyright (c) 2024 Snowapril

#pragma once

#include "vkm/base/common.h"
#include "vkm/renderer/backend/common/render_resource.h"

namespace vkm
{
    class VkmTexture : public VkmRenderResource
    {
    public:
        VkmTexture(VkmDriverBase* driver);
        ~VkmTexture();

    };
} // namespace vkm