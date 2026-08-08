// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmTexture;

    class VkmTextureView : public VkmRenderResource
    {
    public:
        VkmTextureView(VkmDriverBase* driver);
        ~VkmTextureView();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info) = 0;

        inline const VkmTextureViewInfo& getTextureViewInfo() const { return _textureViewInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::TextureView; }

        /*
        * @brief True if this view's parent texture is still live in the resource pool.
        */
        bool isParentAlive() const;

        /*
        * @brief Resolves the parent texture without logging when it is gone.
        * @details Same lookup as resolveParent(), for callers checking liveness rather than
        * expecting the parent to be present.
        * @return The parent texture, or nullptr if it is no longer in the resource pool.
        */
        VkmTexture* tryGetParent() const;

    protected:
        bool initializeTextureViewCommon(VkmResourceHandle handle, const VkmTextureViewInfo& info);

        /*
        * @brief Resolves the parent texture via the resource pool, logging an error if it is
        * not found. Shared by every backend's initialize().
        * @return The parent texture, or nullptr if it is no longer in the resource pool.
        */
        VkmTexture* resolveParent() const;

    protected:
        VkmTextureViewInfo _textureViewInfo;
    };
} // namespace vkm
