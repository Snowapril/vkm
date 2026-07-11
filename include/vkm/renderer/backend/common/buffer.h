// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmBuffer : public VkmRenderResource
    {
    public:
        VkmBuffer(VkmDriverBase* driver);
        ~VkmBuffer();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferInfo& info) = 0;
        virtual bool overrideExternalHandle(void* externalHandle) = 0;

        inline const VkmBufferInfo& getBufferInfo() const { return _bufferInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::Buffer; }

    protected:
        bool initializeBufferCommon(VkmResourceHandle handle, const VkmBufferInfo& info);

    protected:
        VkmBufferInfo _bufferInfo;
    };
} // namespace vkm
