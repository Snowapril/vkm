// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state_object.h>

#include <cstdint>

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

        // MTL4RenderPipelineDescriptor carries no depth/stencil pixel format or fixed-function
        // depth/stencil test state; that is applied at draw time via
        // MTL4RenderCommandEncoder::setDepthStencilState:. Built here from
        // getDescriptor().depthStencilState so recording can bind it without recomputing per frame.
        inline id<MTLDepthStencilState> getDepthStencilState() const { return _depthStencilState; }

        /*
        * @brief The compute function's declared [numthreads(x, y, z)], as recorded in its .vfcache
        * by vkm-compiler. All zero for a graphics pipeline.
        * @details dispatchThreadgroups: needs a threadsPerThreadgroup, and MTLComputePipelineState
        * cannot report what its function declared, so the command buffer reads it from here rather
        * than assuming one engine-wide size. Plain integers rather than an MTLSize, so this header
        * needs nothing from Metal beyond the forward-declared protocols above.
        */
        inline const uint32_t* getComputeThreadGroupSize() const { return _computeThreadGroupSize; }

    protected:
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) override final;
        virtual void destroyInner() override final;

    private:
        id<MTLRenderPipelineState> _renderPipelineState = nullptr;
        id<MTLComputePipelineState> _computePipelineState = nullptr;
        id<MTLDepthStencilState> _depthStencilState = nullptr;
        uint32_t _computeThreadGroupSize[3] = {};
    };
} // namespace vkm
