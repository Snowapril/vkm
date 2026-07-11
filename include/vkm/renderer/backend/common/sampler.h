// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmSampler : public VkmRenderResource
    {
    public:
        VkmSampler(VkmDriverBase* driver);
        ~VkmSampler();

        virtual bool initialize(VkmResourceHandle handle, const VkmSamplerInfo& info) = 0;

        inline const VkmSamplerInfo& getSamplerInfo() const { return _samplerInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::Sampler; }

    protected:
        bool initializeSamplerCommon(VkmResourceHandle handle, const VkmSamplerInfo& info);

    protected:
        VkmSamplerInfo _samplerInfo;
    };
} // namespace vkm
