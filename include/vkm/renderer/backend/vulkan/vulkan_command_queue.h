// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_queue.h>
#include <volk.h>

namespace vkm
{
    class VkmCommandBufferPoolVulkan : public VkmCommandBufferPoolBase
    {
    public:
        VkmCommandBufferPoolVulkan(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue);
        ~VkmCommandBufferPoolVulkan();

    protected:
        virtual VkmCommandBufferBase* newCommandBuffer() override final;
        virtual VKM_COMMAND_BUFFER_HANDLE getOrCreateRHICommandBuffer() override final;

    private:
        VkCommandPool _vkCommandPool{VK_NULL_HANDLE};
    };

    class VkmGpuEventTimelineVulkan : public VkmGpuEventTimelineBase
    {
    public:
        VkmGpuEventTimelineVulkan(VkmDriverBase* driver);
        ~VkmGpuEventTimelineVulkan();

        virtual uint64_t queryLastCompletedTimeline() override final;
        virtual void waitIdle(const uint64_t timeoutMs) override final;

        inline VkSemaphore getTimelineSemaphore() const { return _timelineSemaphore; }

    private:
        VkSemaphore _timelineSemaphore{VK_NULL_HANDLE};
    };

    class VkmCommandQueueVulkan : public VkmCommandQueueBase
    {
    public:
        VkmCommandQueueVulkan(VkmDriverBase* driver);
        ~VkmCommandQueueVulkan();

        inline VkQueue getVkQueue() const { return _vkQueue; }

    public:
        virtual VkmGpuEventTimelineObject submit(const CommandSubmitInfo& submitInfos) override final;
        virtual void setDebugName(const char* name) override final;

    protected:
        virtual bool initializeInner() override final;

    private:
        VkQueue _vkQueue{VK_NULL_HANDLE};
    };
} // namespace vkm
