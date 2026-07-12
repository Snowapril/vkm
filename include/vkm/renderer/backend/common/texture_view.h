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
        * @brief Resolve the parent texture, or nullptr if it's gone. Same lookup as
        * resolveParent() but silent (no error log) -- for callers checking liveness rather
        * than expecting the parent to always be present.
        */
        VkmTexture* tryGetParent() const;

    protected:
        bool initializeTextureViewCommon(VkmResourceHandle handle, const VkmTextureViewInfo& info);

        /*
        * @brief Resolve the parent texture via the resource pool, logging an error if it's
        * not found. Shared by every backend's initialize() -- replaces the identical inline
        * lookup that used to be duplicated per-backend.
        */
        VkmTexture* resolveParent() const;

    protected:
        VkmTextureViewInfo _textureViewInfo;
    };
} // namespace vkm
