// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <volk.h>

namespace vkm
{
    // Vulkan uses a single opaque VkPipeline handle type for both graphics and compute
    // pipelines (unlike Metal's MTLRenderPipelineState/MTLComputePipelineState split).
    // Graphics pipelines target dynamic rendering (VK_KHR_dynamic_rendering) -- no
    // VkRenderPass/VkFramebuffer object is created or required.
    class VkmPipelineStateVulkan : public VkmPipelineStateBase
    {
    public:
        explicit VkmPipelineStateVulkan(VkmDriverBase* driver);
        ~VkmPipelineStateVulkan();

        inline VkPipeline getHandle() const { return _pipeline; }

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        VkPipeline _pipeline{VK_NULL_HANDLE};

        // Resource-binding (descriptor sets / push constants) is a deliberately deferred
        // follow-up (out of scope for this plan) -- this layout is empty, created purely
        // so pipeline creation succeeds.
        VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    };
} // namespace vkm
