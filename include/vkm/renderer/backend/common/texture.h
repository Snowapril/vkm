// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

#include <vector>

namespace vkm
{
    class VkmTextureView;

    class VkmTexture : public VkmRenderResource
    {
    public:
        VkmTexture(VkmDriverBase* driver);
        ~VkmTexture();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureInfo& info) = 0;
        virtual bool overrideExternalHandle(void* externalHandle) = 0;

        inline const VkmTextureInfo& getTextureInfo() const { return _textureInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::Texture; }

        /*
        * @brief Create a view of this texture. This is the ONLY way to create a
        * VkmTextureView -- VkmDriverBase::newTextureView() is protected and friended to
        * this class specifically so callers cannot bypass ownership tracking. info._texture
        * is always overwritten with this texture's own handle, regardless of what the
        * caller passed in.
        */
        VkmTextureView* createView(VkmTextureViewInfo info);

        std::vector<VkmResourceHandle> getOwnedChildHandles() const override { return _ownedViewHandles; }

    protected:
        bool initializeTextureCommon(VkmResourceHandle handle, const VkmTextureInfo& info);

    protected:
        VkmTextureInfo _textureInfo;
        std::vector<VkmResourceHandle> _ownedViewHandles;
    };
} // namespace vkm