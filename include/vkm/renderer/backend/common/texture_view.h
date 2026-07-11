// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmTextureView : public VkmRenderResource
    {
    public:
        VkmTextureView(VkmDriverBase* driver);
        ~VkmTextureView();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info) = 0;

        inline const VkmTextureViewInfo& getTextureViewInfo() const { return _textureViewInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::TextureView; }

    protected:
        bool initializeTextureViewCommon(VkmResourceHandle handle, const VkmTextureViewInfo& info);

    protected:
        VkmTextureViewInfo _textureViewInfo;
    };
} // namespace vkm
