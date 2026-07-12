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

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) override final;
        virtual void onEndRenderPass() override final;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) override final;
        virtual void onUnbindPipeline() override final;

    private:
        VkCommandBuffer _vkCommandBuffer{VK_NULL_HANDLE};
    };
} // namespace vkm
