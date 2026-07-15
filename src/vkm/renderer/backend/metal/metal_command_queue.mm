// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/common/gpu_crash_handler.h>

#import <Metal/MTL4CommandQueue.h>
#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4CommandAllocator.h>
#import <Metal/MTL4CommitFeedback.h>

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
        // Wait until the last ALLOCATED (submitted) value signals, mirroring
        // VkmGpuEventTimelineVulkan::waitIdle -- waiting on the cached completed value
        // returns immediately without waiting for in-flight work.
        [_mtlSharedEvent waitUntilSignaledValue:_lastAllocatedTimeline timeoutMS:timeoutMs];
        _lastCompletedCachedTimeline = [_mtlSharedEvent signaledValue];
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

        VkmRenderResourcePoolMetal* renderResourcePoolMetal = static_cast<VkmRenderResourcePoolMetal*>(_driver->getRenderResourcePool());
        renderResourcePoolMetal->commitPendingResidencyChanges();

        const VkmGpuEventTimelineObject timelineObject(gpuEventTimelineMetal, lastSubmittedTimelineValue);
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        _driver->getGpuCrashHandler()->recordSubmission(this, submitInfos, timelineObject);
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        // MTL4's per-submission error reporting: register a feedback handler on this commit's
        // options. Metal invokes it on the queue's feedbackQueue -- an internal *serial*
        // dispatch queue by default -- once the committed work completes; commitFeedback.error
        // is non-nil when the GPU encountered an issue running it (see MTL4CommandQueueError,
        // which includes MTL4CommandQueueErrorDeviceRemoved). reportCrash() does real work
        // (mutex lock, string formatting, and VKM_DEBUG_ERROR's live stack-unwind), so it must
        // never run synchronously on that serial queue: doing so was observed to block/delay
        // delivery of later commits' feedback long enough to itself trigger
        // MTL4CommandQueueErrorTimeout on otherwise-healthy frames, turning one real error into
        // a cascade. Dispatching it to a separate queue keeps the feedback handler itself fast
        // regardless of how expensive crash reporting is. The captured driver pointer must
        // outlive the async callback -- true under normal shutdown, where GPU work is drained
        // before the driver is torn down.
        VkmDriverBase* driver = _driver;
        MTL4CommitOptions* commitOptions = [[MTL4CommitOptions alloc] init];
        [commitOptions addFeedbackHandler:^(id<MTL4CommitFeedback> commitFeedback) {
            NSError* error = commitFeedback.error;
            if (error != nil)
            {
                const std::string errorCode = fmt::format("{}({})", error.domain.UTF8String, static_cast<long>(error.code));
                const std::string reason = error.localizedDescription != nil ? error.localizedDescription.UTF8String : "";
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    driver->getGpuCrashHandler()->reportCrash("Metal", errorCode, reason);
                });
            }
        }];

        [_mtlCommandQueue commit:mtlCommandBuffers count:count options:commitOptions];
        [_mtlCommandQueue signalEvent:mtlSharedEvent value:lastSubmittedTimelineValue];

        return timelineObject;
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

        VkmRenderResourcePoolMetal* renderResourcePoolMetal = static_cast<VkmRenderResourcePoolMetal*>(driverMetal->getRenderResourcePool());
        id<MTLResidencySet> residencySet = renderResourcePoolMetal->getResidencySet(VkmResourcePoolType::Default);
        if (residencySet == nil)
        {
            // Without a residency set no resource is ever made resident on this queue --
            // fail loudly at init instead of faulting at first GPU use.
            VKM_DEBUG_ERROR("Residency set unavailable; cannot initialize Metal command queue");
            return false;
        }
        [_mtlCommandQueue addResidencySet:residencySet];

        _commandBufferPool.reset(new VkmCommandBufferPoolMetal(_driver, this));
        _gpuEventTimeline.reset(new VkmGpuEventTimelineMetal(_driver));

        return _mtlCommandQueue != nil;
    }
}
