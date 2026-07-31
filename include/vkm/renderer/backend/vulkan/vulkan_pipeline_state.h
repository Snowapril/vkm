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
        inline VkPipelineLayout getPipelineLayout() const { return _pipelineLayout; }

        /*
        * @brief This pipeline's descriptor set 2 layout, or VK_NULL_HANDLE when it declares no
        * per-pass resources.
        *
        * Unlike sets 0 and 1 -- which every pipeline shares, and which therefore live on the
        * bindless and frame-constant managers -- set 2's layout is built from this pipeline's own
        * `perPassResources` declaration, so it is owned here. VkmPerPassResourceTableVulkan
        * allocates its descriptor set from it.
        */
        inline VkDescriptorSetLayout getPerPassSetLayout() const { return _perPassSetLayout; }

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        VkPipeline _pipeline{VK_NULL_HANDLE};

        // Every Vulkan pipeline shares the engine-global bindless set 0 (see
        // VkmBindlessResourceManagerVulkan) plus a small push-constant range carrying the
        // current draw's bindless slot indices. Sets 1-3 ("engine managed", per TODO.md)
        // remain unreserved/deferred follow-up work.
        VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout _perPassSetLayout{VK_NULL_HANDLE};
    };
} // namespace vkm
