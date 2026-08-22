// Copyright (c) 2026 Snowapril

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
        * @brief Hands a no-longer-current VkCommandBuffer back, with the timeline object of the
        * submission that last used it.
        * @details A later getOrCreateRHICommandBuffer() returns it to the reuse pool once that
        * timeline value has completed. Not recycled eagerly: resetting or re-recording a command
        * buffer still in the pending state is a validation error, and the render graph releases a
        * command buffer back to the pool in the same frame it submits it, up to FRAME_COUNT frames
        * before that submission completes. Callers must hold the pool's command-buffer mutex, which
        * VkmCommandBufferPoolBase::allocate() already does across the whole handoff.
        */
        void retireRHICommandBuffer(VkCommandBuffer commandBuffer, const VkmGpuEventTimelineObject& timelineObject);

    protected:
        virtual VkmCommandBufferBase* newCommandBuffer() override final;
        virtual VKM_COMMAND_BUFFER_HANDLE getOrCreateRHICommandBuffer() override final;

    private:
        VkCommandPool _vkCommandPool{VK_NULL_HANDLE};
        // Handed back but possibly still executing. Bounded by the frames in flight: an entry
        // moves to _availableCommandBuffers as soon as its submission completes.
        std::vector<std::pair<VkCommandBuffer, VkmGpuEventTimelineObject>> _retiredCommandBuffers;
        // Completed and ready to be handed out again. Together with the list above this caps
        // the pool at the peak number of simultaneously-live command buffers, so steady-state
        // rendering allocates none.
        std::vector<VkCommandBuffer> _availableCommandBuffers;
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
