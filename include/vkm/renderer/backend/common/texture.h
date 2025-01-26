// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmTexture : public VkmRenderResource
    {
    public:
        VkmTexture(VkmDriverBase* driver);
        ~VkmTexture();

        virtual bool initialize(const VkmTextureInfo& info) = 0;
        virtual bool overrideExternalHandle(void* externalHandle) = 0;

    protected:
        bool initializeCommon(const VkmTextureInfo& info);

    protected:
        VkmTextureInfo _textureInfo;
    };
} // namespace vkm