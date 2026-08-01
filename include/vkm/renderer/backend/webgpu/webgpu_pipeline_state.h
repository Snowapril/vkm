// Copyright (c) 2025 Snowapril

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
        * @brief This pipeline's bind group 2 layout, or null when it declares no per-pass
        * resources. Owned here because, unlike groups 0 and 1, it is built from this pipeline's
        * own declaration; VkmPerPassResourceTableWebGPU creates its bind group from it.
        */
        inline WGPUBindGroupLayout getPerPassBindGroupLayout() const { return _perPassBindGroupLayout; }

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        WGPURenderPipeline _renderPipeline{nullptr};
        WGPUComputePipeline _computePipeline{nullptr};
        WGPUBindGroupLayout _perPassBindGroupLayout{nullptr};
    };
} // namespace vkm
