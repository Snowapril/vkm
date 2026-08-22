// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmPipelineStateWebGPU final : public VkmPipelineStateBase
    {
    public:
        explicit VkmPipelineStateWebGPU(VkmDriverBase* driver);
        ~VkmPipelineStateWebGPU();

        inline WGPURenderPipeline getRenderPipeline() const { return _renderPipeline; }
        inline WGPUComputePipeline getComputePipeline() const { return _computePipeline; }

        /*
        * @brief This pipeline's layout for one PSO-declared bind group, or null when it declares
        * nothing there. Owned here because, unlike groups 0 and 1, these are built from this
        * pipeline's own declarations; VkmResourceTableWebGPU creates its bind group from one.
        */
        inline WGPUBindGroupLayout getBindGroupLayout(VkmResourceSetKind kind) const
        {
            return (kind == VkmResourceSetKind::PerPass) ? _perPassBindGroupLayout : _perDrawBindGroupLayout;
        }

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        WGPURenderPipeline _renderPipeline{nullptr};
        WGPUComputePipeline _computePipeline{nullptr};
        WGPUBindGroupLayout _perPassBindGroupLayout{nullptr};
        WGPUBindGroupLayout _perDrawBindGroupLayout{nullptr};
        // Empty stand-in for group 2 when only group 3 is declared -- a group must land at its own
        // index, and the pipeline layout's array is positional.
        WGPUBindGroupLayout _emptyBindGroupLayout{nullptr};
    };
} // namespace vkm
