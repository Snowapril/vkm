// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#import <Metal/MTL4CommandQueue.h>
#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4CommandAllocator.h>

namespace vkm
{
    VkmCommandBufferPoolMetal::VkmCommandBufferPoolMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue)
        : VkmCommandBufferPoolBase(driver, commandQueue)
    {
        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(driver);
        _commandAllocator = [driverMetal->getMTLDevice() newCommandAllocator];
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
        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
        id<MTL4CommandBuffer> mtlCommandBuffer = [driverMetal->getMTLDevice() newCommandBuffer];
        [mtlCommandBuffer beginCommandBufferWithAllocator:_commandAllocator];
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

        const NSUInteger count = static_cast<NSUInteger>(std::min(submitInfos.commandBufferCount, MAX_NUM_COMMAND_BUFFER_SUBMITS));
        id<MTL4CommandBuffer> mtlCommandBuffers[MAX_NUM_COMMAND_BUFFER_SUBMITS];

        for (uint32_t i = 0; i < count; ++i)
        {
            VkmCommandBufferMetal* commandBufferMetal = static_cast<VkmCommandBufferMetal*>(submitInfos.commandBuffers[i]);
            id<MTL4CommandBuffer> mtlCommandBuffer = commandBufferMetal->getMTLCommandBuffer();

            const VkmGpuEventTimelineObject& mtlGpuEvent = commandBufferMetal->getGpuEventTimelineObject();

            VKM_ASSERT(mtlGpuEvent._gpuEventTimeline == gpuEventTimelineMetal,
                       "The GPU event timeline object must be from the same command queue as the command buffer.");

            lastSubmittedTimelineValue = std::max(lastSubmittedTimelineValue, mtlGpuEvent._timelineValue);

            [mtlCommandBuffer endCommandBuffer];
            mtlCommandBuffers[i] = mtlCommandBuffer;
        }

        [_mtlCommandQueue commit:mtlCommandBuffers count:count];
        [_mtlCommandQueue signalEvent:mtlSharedEvent value:lastSubmittedTimelineValue];

        return VkmGpuEventTimelineObject(gpuEventTimelineMetal, lastSubmittedTimelineValue);
    }

    void VkmCommandQueueMetal::setDebugName(const char* name)
    {
        // MTL4CommandQueue.label is read-only; set via MTL4CommandQueueDescriptor at creation time.
        (void)name;
    }

    bool VkmCommandQueueMetal::initializeInner()
    {
        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
        _mtlCommandQueue = [driverMetal->getMTLDevice() newMTL4CommandQueue];

        _commandBufferPool.reset(new VkmCommandBufferPoolMetal(_driver, this));
        _gpuEventTimeline.reset(new VkmGpuEventTimelineMetal(_driver));

        return _mtlCommandQueue != nil;
    }
}
