// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <volk.h>

namespace vkm
{
    /*
    * @brief A Vulkan graphics or compute pipeline.
    * @details Vulkan uses one opaque VkPipeline handle type for both, unlike Metal's
    * MTLRenderPipelineState/MTLComputePipelineState split. Graphics pipelines target dynamic
    * rendering, so no VkRenderPass or VkFramebuffer object is created or required.
    */
    class VkmPipelineStateVulkan : public VkmPipelineStateBase
    {
    public:
        explicit VkmPipelineStateVulkan(VkmDriverBase* driver);
        ~VkmPipelineStateVulkan();

        inline VkPipeline getHandle() const { return _pipeline; }
        inline VkPipelineLayout getPipelineLayout() const { return _pipelineLayout; }

        /*
        * @brief This pipeline's layout for one PSO-declared set.
        * @details Sets 0 and 1 are shared by every pipeline and live on the bindless and
        * frame-constant managers; these are built from this pipeline's own `perPassResources` /
        * `perDrawResources` declarations, so they are owned here. VkmResourceTableVulkan allocates
        * its descriptor set from the matching one.
        * @param kind Which PSO-declared set to report.
        * @return The layout, or VK_NULL_HANDLE when the pipeline declares nothing there.
        */
        inline VkDescriptorSetLayout getSetLayout(VkmResourceSetKind kind) const
        {
            return (kind == VkmResourceSetKind::PerPass) ? _perPassSetLayout : _perDrawSetLayout;
        }

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        VkPipeline _pipeline{VK_NULL_HANDLE};

        // Every Vulkan pipeline shares the engine-global bindless set 0 (see
        // VkmBindlessResourceManagerVulkan) and per-frame set 1, plus a small push-constant range.
        // Sets 2 and 3 are this pipeline's own; see the layouts below.
        VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout _perPassSetLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout _perDrawSetLayout{VK_NULL_HANDLE};
        // Empty stand-in for a set this pipeline skipped but a later set needs to sit above -- a
        // pipeline declaring set 3 and not set 2 still has to put set 3 at index 3.
        VkDescriptorSetLayout _emptySetLayout{VK_NULL_HANDLE};
    };
} // namespace vkm
