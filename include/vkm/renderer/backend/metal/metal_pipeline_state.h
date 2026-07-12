// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state_object.h>

@protocol MTLRenderPipelineState;
@protocol MTLComputePipelineState;
@protocol MTLDepthStencilState;

namespace vkm
{
    class VkmPipelineStateMetal : public VkmPipelineStateBase
    {
    public:
        explicit VkmPipelineStateMetal(VkmDriverBase* driver);
        ~VkmPipelineStateMetal();

        inline id<MTLRenderPipelineState> getRenderPipelineState() const { return _renderPipelineState; }
        inline id<MTLComputePipelineState> getComputePipelineState() const { return _computePipelineState; }

        // Metal has no per-pipeline depth/stencil pixel-format or fixed-function
        // depth/stencil test state on MTL4RenderPipelineDescriptor -- depth/stencil
        // comparison/stencil-op state is applied at draw time via
        // MTL4RenderCommandEncoder::setDepthStencilState:. Built here (from
        // getDescriptor().depthStencilState) so future draw-call recording code can
        // bind it without recomputing it per-frame.
        inline id<MTLDepthStencilState> getDepthStencilState() const { return _depthStencilState; }

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        id<MTLRenderPipelineState> _renderPipelineState = nullptr;
        id<MTLComputePipelineState> _computePipelineState = nullptr;
        id<MTLDepthStencilState> _depthStencilState = nullptr;
    };
} // namespace vkm
