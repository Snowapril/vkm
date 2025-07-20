// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_queue.h>
#include <vector>

@protocol MTLCommandQueue;
@protocol MTLSharedEvent;

namespace vkm
{
    /*
    * @brief Command buffer ppol base class
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
    * @brief Command queue 
    */
    class VkmCommandQueueMetal : public VkmCommandQueueBase
    {
    public:
        VkmCommandQueueMetal(VkmDriverBase* driver);
        ~VkmCommandQueueMetal();

        inline id<MTLCommandQueue> getMTLCommandQueue() const { return _mtlCommandQueue; }
    
    public:
        virtual VkmGpuEventTimelineObject submit(const CommandSubmitInfo& submitInfos) override final;
        virtual void setDebugName(const char* name) override final;

    protected:
        virtual bool initializeInner() override final;

    protected:
        id<MTLCommandQueue> _mtlCommandQueue;
        std::vector<VkmCommandBufferBase*> _commandBuffersSubmitted;
    };
}