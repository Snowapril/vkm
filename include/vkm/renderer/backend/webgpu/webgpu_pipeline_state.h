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

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        WGPURenderPipeline _renderPipeline{nullptr};
        WGPUComputePipeline _computePipeline{nullptr};
    };
} // namespace vkm
