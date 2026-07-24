// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>
#include <volk.h>

namespace vkm
{
    class VkmCommandBufferVulkan : public VkmCommandBufferBase
    {
    public:
        VkmCommandBufferVulkan(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferVulkan();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) override final;

        inline VkCommandBuffer getVkCommandBuffer() const { return _vkCommandBuffer; }

        virtual void writeGpuTimestampBegin() override final;
        virtual void writeGpuTimestampEnd() override final;

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) override final;
        virtual void onEndRenderPass() override final;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) override final;
        virtual void onUnbindPipeline() override final;
        virtual void onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override final;
        virtual void onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset) override final;
        virtual void onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture) override final;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override final;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) override final;
        virtual void onSetDebugName(const char* name) override final;
        virtual void onPushDebugGroup(const char* name) override final;
        virtual void onPopDebugGroup() override final;
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) override final;
        virtual void onEndCommandBuffer() override final;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    private:
        VkCommandBuffer _vkCommandBuffer{VK_NULL_HANDLE};

        // Set in onBindPipeline(); onSetPushConstants() needs the bound pipeline's layout,
        // but VkmCommandBufferBase::_boundPipelineState is private to the base class.
        VkPipelineLayout _boundPipelineLayout{VK_NULL_HANDLE};
    };
} // namespace vkm
