// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmBuffer;

    class VkmBufferView : public VkmRenderResource
    {
    public:
        VkmBufferView(VkmDriverBase* driver);
        ~VkmBufferView();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferViewInfo& info) = 0;

        inline const VkmBufferViewInfo& getBufferViewInfo() const { return _bufferViewInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::BufferView; }

        /*
        * @brief True if this view's parent buffer is still live in the resource pool.
        */
        bool isParentAlive() const;

        /*
        * @brief Resolves the parent buffer without logging when it is gone.
        * @details Same lookup as resolveParent(), for callers checking liveness rather than
        * expecting the parent to be present.
        * @return The parent buffer, or nullptr if it is no longer in the resource pool.
        */
        VkmBuffer* tryGetParent() const;

    protected:
        bool initializeBufferViewCommon(VkmResourceHandle handle, const VkmBufferViewInfo& info);

        /*
        * @brief Resolves the parent buffer via the resource pool, logging an error if it is
        * not found. Shared by every backend's initialize().
        * @return The parent buffer, or nullptr if it is no longer in the resource pool.
        */
        VkmBuffer* resolveParent() const;

    protected:
        VkmBufferViewInfo _bufferViewInfo;
    };
} // namespace vkm
