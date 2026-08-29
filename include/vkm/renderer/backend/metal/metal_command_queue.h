// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_queue.h>
#include <vector>

@protocol MTL4CommandQueue;
@protocol MTL4CommandAllocator;
@protocol MTLSharedEvent;

namespace vkm
{
    /*
    * @brief Metal command buffer pool
    */
    class VkmCommandBufferPoolMetal : public VkmCommandBufferPoolBase
    {
    public:
        VkmCommandBufferPoolMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue);
        ~VkmCommandBufferPoolMetal();

    protected:
        virtual VKM_COMMAND_BUFFER_HANDLE getOrCreateRHICommandBuffer() override final;

    protected:
        virtual VkmCommandBufferBase* newCommandBuffer() override final;

    private:
        id<MTL4CommandAllocator> _commandAllocator;
    };

    /*
    * @brief GPU event timeline for Metal
    */
    class VkmGpuEventTimelineMetal : public VkmGpuEventTimelineBase
    {
    public:
        VkmGpuEventTimelineMetal(VkmDriverBase* driver);
        ~VkmGpuEventTimelineMetal();

        virtual uint64_t queryLastCompletedTimeline() override final;
        virtual void waitIdle( const uint64_t timeoutMs ) override final;

        inline id<MTLSharedEvent> getMTLSharedEvent() const { return _mtlSharedEvent; }

    private:
        id<MTLSharedEvent> _mtlSharedEvent; // Metal shared event for GPU/CPU synchronization
    };

    /*
    * @brief Metal command queue
    */
    class VkmCommandQueueMetal : public VkmCommandQueueBase
    {
    public:
        VkmCommandQueueMetal(VkmDriverBase* driver);
        ~VkmCommandQueueMetal();

        inline id<MTL4CommandQueue> getMTLCommandQueue() const { return _mtlCommandQueue; }

    public:
        virtual void setDebugName(const char* name) override final;

    protected:
        virtual VkmGpuEventTimelineObject submitInner(const CommandSubmitInfo& submitInfos) override final;
        virtual bool initializeInner() override final;

    protected:
        id<MTL4CommandQueue> _mtlCommandQueue;
        std::vector<VkmCommandBufferBase*> _commandBuffersSubmitted;
    };
}