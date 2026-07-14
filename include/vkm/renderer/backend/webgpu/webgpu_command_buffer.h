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
        // Valid only between onBindPipeline(compute)/onUnbindPipeline.
        inline WGPUComputePassEncoder getActiveComputePassEncoder() const { return _computePassEncoder; }

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) override final;
        virtual void onEndRenderPass() override final;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) override final;
        virtual void onUnbindPipeline() override final;
        virtual void onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override final;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override final;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) override final;
        virtual void onSetDebugName(const char* name) override final;
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) override final;
        virtual void onEndCommandBuffer() override final;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    private:
        WGPUCommandEncoder _encoder{nullptr};
        WGPURenderPassEncoder _renderPassEncoder{nullptr};
        WGPUComputePassEncoder _computePassEncoder{nullptr};
    };
} // namespace vkm
