// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include <Metal/MTLCommandQueue.h>

namespace vkm
{
    VkmCommandBufferPoolMetal::VkmCommandBufferPoolMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue)
        : VkmCommandBufferPoolBase(driver, commandQueue)
    {

    }

    VkmCommandBufferPoolMetal::~VkmCommandBufferPoolMetal()
    {

    }

    VkmCommandBufferBase* VkmCommandBufferPoolMetal::newCommandBuffer()
    {
        VkmCommandBufferMetal* commandBufferMetal = new VkmCommandBufferMetal(_driver, _commandQueue, this);
        return commandBufferMetal;
    }

    VKM_COMMAND_BUFFER_HANDLE VkmCommandBufferPoolMetal::getOrCreateRHICommandBuffer()
    {
        VkmCommandQueueMetal* commandQueueMetal = static_cast<VkmCommandQueueMetal*>(_commandQueue);
        id<MTLCommandBuffer> mtlCommandBuffer = [commandQueueMetal->getMTLCommandQueue() commandBuffer];
        return (__bridge VKM_COMMAND_BUFFER_HANDLE)mtlCommandBuffer;
    }

    VkmGpuEventTimelineMetal::VkmGpuEventTimelineMetal(VkmDriverBase* driver)
        : VkmGpuEventTimelineBase(driver)
    {
        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(driver);
        _mtlSharedEvent = [driverMetal->getMTLDevice() newSharedEvent];
    }

    VkmGpuEventTimelineMetal::~VkmGpuEventTimelineMetal()
    {
        
    }
    
    uint64_t VkmGpuEventTimelineMetal::queryLastCompletedTimeline()
    {
        _lastCompletedCachedTimeline = [_mtlSharedEvent signaledValue];
        return _lastCompletedCachedTimeline;
    }

    void VkmGpuEventTimelineMetal::waitIdle(const uint64_t timeoutMs)
    {
        [_mtlSharedEvent waitUntilSignaledValue:_lastCompletedCachedTimeline timeoutMS:timeoutMs];
    }

    VkmCommandQueueMetal::VkmCommandQueueMetal(VkmDriverBase* driver)
        : VkmCommandQueueBase(driver)
    {

    }

    VkmCommandQueueMetal::~VkmCommandQueueMetal()
    {

    }
    
    VkmGpuEventTimelineObject VkmCommandQueueMetal::submit(const CommandSubmitInfo& submitInfos)
    {
        VkmGpuEventTimelineMetal* gpuEventTimelineMetal = static_cast<VkmGpuEventTimelineMetal*>(_gpuEventTimeline.get());
        id<MTLSharedEvent> mtlSharedEvent = gpuEventTimelineMetal->getMTLSharedEvent();
        uint64_t lastSubmittedTimelineValue = 0;

        for (uint32_t i = 0; i < std::min(submitInfos.commandBufferCount, MAX_NUM_COMMAND_BUFFER_SUBMITS); ++i)
        {
            VkmCommandBufferMetal* commandBufferMetal = static_cast<VkmCommandBufferMetal*>(submitInfos.commandBuffers[i]);
            id<MTLCommandBuffer> mtlCommandBuffer = commandBufferMetal->getMTLCommandBuffer();

            const VkmGpuEventTimelineObject& mtlGpuEvent = commandBufferMetal->getGpuEventTimelineObject();

            VKM_ASSERT(mtlGpuEvent._gpuEventTimeline == gpuEventTimelineMetal,
                       "The GPU event timeline object must be from the same command queue as the command buffer.");
            
            // Ensure timeline value is incremental and contiguous
            lastSubmittedTimelineValue = std::max(lastSubmittedTimelineValue, mtlGpuEvent._timelineValue);

            [mtlCommandBuffer encodeSignalEvent:mtlSharedEvent value:mtlGpuEvent._timelineValue];
            [mtlCommandBuffer commit];
        }
        return VkmGpuEventTimelineObject(gpuEventTimelineMetal, lastSubmittedTimelineValue);
    }

    void VkmCommandQueueMetal::setDebugName(const char* name)
    {
        [_mtlCommandQueue setLabel:[NSString stringWithUTF8String:name]];
    }

    bool VkmCommandQueueMetal::initializeInner()
    {
        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
        _mtlCommandQueue = [driverMetal->getMTLDevice() newCommandQueue];

        _commandBufferPool.reset(new VkmCommandBufferPoolMetal(_driver, this));
        _gpuEventTimeline.reset(new VkmGpuEventTimelineMetal(_driver));

        return _mtlCommandQueue != nil;
    }
}
