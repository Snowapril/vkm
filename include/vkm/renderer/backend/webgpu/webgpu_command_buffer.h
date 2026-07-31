// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>
#include <webgpu/webgpu.h>

#include <vector>

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
        virtual void onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset, uint32_t arrayLayer) override final;
        virtual void onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture) override final;
        virtual void onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture, uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer) override final;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override final;
        virtual void onDrawIndirectCount(VkmIndirectArgumentLayout layout,
                                         VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                         VkmResourceHandle countBuffer, uint64_t countOffset,
                                         uint32_t maxDrawCount) override final;
        virtual void onDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override final;
        virtual void onBarrierIndirectArgumentBuffer(VkmResourceHandle buffer) override final;
        virtual void onBarrierTextureForShaderRead(VkmResourceHandle texture) override final;
        virtual void onBindPerPassResources(VkmPerPassResourceTableBase* table) override final;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) override final;
        virtual void onSetDebugName(const char* name) override final;
        virtual void onPushDebugGroup(const char* name) override final;
        virtual void onPopDebugGroup() override final;
        virtual void onBeginCommandBuffer() override final;
        virtual void onBeginGpuZone(uint32_t beginSlot, uint32_t endSlot) override final;
        virtual bool onEndGpuZone() override final;
        virtual void onResolveGpuZones(uint32_t firstSlot, uint32_t count) override final;
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) override final;
        virtual void onEndCommandBuffer() override final;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    private:
        /*
        * @brief Fills `outTimestampWrites` from the innermost GPU zone that has not yet been
        * given a pass to write into, and returns whether there was one.
        *
        * WebGPU has no encoder-level timestamp write, so a zone's begin/end pair can only be
        * carried by one render or compute pass descriptor. The innermost open zone wins, which
        * is what makes a subgraph's zone -- rather than the outer submission-wide one -- the one
        * that gets measured.
        */
        bool takePendingGpuZone(WGPUPassTimestampWrites* outTimestampWrites);

        WGPUCommandEncoder _encoder{nullptr};
        WGPURenderPassEncoder _renderPassEncoder{nullptr};
        WGPUComputePassEncoder _computePassEncoder{nullptr};

        // GPU zones currently open, innermost last. `_attached` flips once a pass has taken the
        // pair, which is what endGpuZone() reports back so the profiler can drop a zone that
        // enclosed no pass at all.
        struct OpenGpuZone
        {
            uint32_t _beginSlot = 0;
            uint32_t _endSlot = 0;
            bool _attached = false;
        };
        std::vector<OpenGpuZone> _openGpuZones;
    };
} // namespace vkm
