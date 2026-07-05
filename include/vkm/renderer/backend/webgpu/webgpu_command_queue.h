// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_queue.h>
#include <webgpu/webgpu.h>

#include <atomic>

namespace vkm
{
    class VkmCommandBufferPoolWebGPU final : public VkmCommandBufferPoolBase
    {
    public:
        VkmCommandBufferPoolWebGPU(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue);
        ~VkmCommandBufferPoolWebGPU();

    protected:
        virtual VkmCommandBufferBase* newCommandBuffer() override final;
        virtual VKM_COMMAND_BUFFER_HANDLE getOrCreateRHICommandBuffer() override final;
    };

    // WebGPU has no native fence/semaphore object; completion is tracked purely via
    // wgpuQueueOnSubmittedWorkDone callbacks driven by the browser event loop.
    class VkmGpuEventTimelineWebGPU final : public VkmGpuEventTimelineBase
    {
    public:
        VkmGpuEventTimelineWebGPU(VkmDriverBase* driver);
        ~VkmGpuEventTimelineWebGPU();

        virtual uint64_t queryLastCompletedTimeline() override final;
        virtual void waitIdle(const uint64_t timeoutMs) override final;

        void onSubmittedWorkDone(uint64_t timelineValue);

    private:
        std::atomic<uint64_t> _lastCompletedTimeline{0};
    };

    class VkmCommandQueueWebGPU final : public VkmCommandQueueBase
    {
    public:
        VkmCommandQueueWebGPU(VkmDriverBase* driver);
        ~VkmCommandQueueWebGPU();

        inline WGPUQueue getWGPUQueue() const { return _wgpuQueue; }

    public:
        virtual VkmGpuEventTimelineObject submit(const CommandSubmitInfo& submitInfos) override final;
        virtual void setDebugName(const char* name) override final;

    protected:
        virtual bool initializeInner() override final;

    private:
        WGPUQueue _wgpuQueue{nullptr};
    };
} // namespace vkm
