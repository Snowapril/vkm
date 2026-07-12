// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/sampler.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmSamplerWebGPU final : public VkmSampler
    {
    public:
        VkmSamplerWebGPU(VkmDriverBase* driver);
        ~VkmSamplerWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmSamplerInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline WGPUSampler getSampler() const { return _wgpuSampler; }

    private:
        WGPUSampler _wgpuSampler{nullptr};
    };
} // namespace vkm
