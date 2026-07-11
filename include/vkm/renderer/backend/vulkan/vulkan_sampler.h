// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/sampler.h>
#include <volk.h>

namespace vkm
{
    class VkmSamplerVulkan : public VkmSampler
    {
    public:
        VkmSamplerVulkan(VkmDriverBase* driver);
        ~VkmSamplerVulkan();

        virtual bool initialize(VkmResourceHandle handle, const VkmSamplerInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline VkSampler getSampler() const { return _vkSampler; }

    private:
        VkSampler _vkSampler{VK_NULL_HANDLE};
    };
} // namespace vkm
