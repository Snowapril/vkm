// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/sampler.h>

@protocol MTLSamplerState;

namespace vkm
{
    class VkmSamplerMetal : public VkmSampler
    {
    public:
        VkmSamplerMetal(VkmDriverBase* driver);
        ~VkmSamplerMetal();

        virtual bool initialize(VkmResourceHandle handle, const VkmSamplerInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline id<MTLSamplerState> getSampler() const { return _mtlSampler; }

    private:
        id<MTLSamplerState> _mtlSampler{nullptr};
    };
} // namespace vkm
