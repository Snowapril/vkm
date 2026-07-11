// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmBufferView : public VkmRenderResource
    {
    public:
        VkmBufferView(VkmDriverBase* driver);
        ~VkmBufferView();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferViewInfo& info) = 0;

        inline const VkmBufferViewInfo& getBufferViewInfo() const { return _bufferViewInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::BufferView; }

    protected:
        bool initializeBufferViewCommon(VkmResourceHandle handle, const VkmBufferViewInfo& info);

    protected:
        VkmBufferViewInfo _bufferViewInfo;
    };
} // namespace vkm
