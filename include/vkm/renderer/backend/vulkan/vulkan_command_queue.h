// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_queue.h>
#include <volk.h>

#include <utility>
#include <vector>

namespace vkm
{
    class VkmCommandBufferPoolVulkan : public VkmCommandBufferPoolBase
    {
    public:
        VkmCommandBufferPoolVulkan(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue);
        ~VkmCommandBufferPoolVulkan();

        /*
        * @brief Hands a no-longer-current VkCommandBuffer back, together with the timeline
        * object of the submission that last used it. It is freed by a later
        * getOrCreateRHICommandBuffer() once that timeline value has completed.
        * @details Not freed eagerly on purpose: vkFreeCommandBuffers on a command buffer still
        * in the pending state is a validation error, and the render graph releases a command
        * buffer back to the pool in the same frame it submits it -- up to FRAME_COUNT frames
        * before that submission completes. Callers must hold the pool's command-buffer mutex,
        * which VkmCommandBufferPoolBase::allocate() already does across the whole handoff.
        */
        void retireRHICommandBuffer(VkCommandBuffer commandBuffer, const VkmGpuEventTimelineObject& timelineObject);

    protected:
        virtual VkmCommandBufferBase* newCommandBuffer() override final;
        virtual VKM_COMMAND_BUFFER_HANDLE getOrCreateRHICommandBuffer() override final;

    private:
        VkCommandPool _vkCommandPool{VK_NULL_HANDLE};
        // Bounded by the frames in flight: an entry is freed as soon as its submission
        // completes, so this never grows past the number of uncompleted submissions.
        std::vector<std::pair<VkCommandBuffer, VkmGpuEventTimelineObject>> _retiredCommandBuffers;
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
