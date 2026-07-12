// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmCommandBufferWebGPU final : public VkmCommandBufferBase
    {
    public:
        VkmCommandBufferWebGPU(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferWebGPU();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) override final;

        // VKM_COMMAND_BUFFER_HANDLE maps to this encoder until wgpuCommandEncoderFinish is
        // called at submit time (WebGPU has no separate pre-allocated "command buffer" step).
        inline WGPUCommandEncoder getWGPUCommandEncoder() const { return _encoder; }
        // Valid only between onBeginRenderPass/onEndRenderPass.
        inline WGPURenderPassEncoder getActiveRenderPassEncoder() const { return _renderPassEncoder; }

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) override final;
        virtual void onEndRenderPass() override final;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) override final;
        virtual void onUnbindPipeline() override final;

    private:
        WGPUCommandEncoder _encoder{nullptr};
        WGPURenderPassEncoder _renderPassEncoder{nullptr};
    };
} // namespace vkm
