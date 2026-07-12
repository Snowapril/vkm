// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_command_queue.h>
#include <vkm/renderer/backend/webgpu/webgpu_command_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/common/gpu_crash_handler.h>

#include <emscripten/emscripten.h>
#include <vector>

namespace vkm
{
    namespace
    {
        struct SubmitWorkDoneContext
        {
            VkmGpuEventTimelineWebGPU* timeline;
            uint64_t timelineValue;
        };

        void onQueueWorkDone(WGPUQueueWorkDoneStatus status, WGPUStringView message, void* userdata1, void* userdata2)
        {
            (void)userdata2;
            SubmitWorkDoneContext* context = static_cast<SubmitWorkDoneContext*>(userdata1);
            if (status != WGPUQueueWorkDoneStatus_Success)
            {
                VKM_DEBUG_ERROR(fmt::format("WebGPU submitted work failed: {}", toStdString(message)).c_str());
            }
            context->timeline->onSubmittedWorkDone(context->timelineValue);
            delete context;
        }
    } // namespace

    // VkmCommandBufferPoolWebGPU

    VkmCommandBufferPoolWebGPU::VkmCommandBufferPoolWebGPU(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue)
        : VkmCommandBufferPoolBase(driver, commandQueue)
    {
    }

    VkmCommandBufferPoolWebGPU::~VkmCommandBufferPoolWebGPU()
    {
    }

    VkmCommandBufferBase* VkmCommandBufferPoolWebGPU::newCommandBuffer()
    {
        return new VkmCommandBufferWebGPU(_driver, _commandQueue, this);
    }

    VKM_COMMAND_BUFFER_HANDLE VkmCommandBufferPoolWebGPU::getOrCreateRHICommandBuffer()
    {
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        const WGPUCommandEncoderDescriptor encoderDesc{};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(driverWebGPU->getDevice(), &encoderDesc);
        return static_cast<VKM_COMMAND_BUFFER_HANDLE>(encoder);
    }

    // VkmGpuEventTimelineWebGPU

    VkmGpuEventTimelineWebGPU::VkmGpuEventTimelineWebGPU(VkmDriverBase* driver)
        : VkmGpuEventTimelineBase(driver)
    {
    }

    VkmGpuEventTimelineWebGPU::~VkmGpuEventTimelineWebGPU()
    {
    }

    uint64_t VkmGpuEventTimelineWebGPU::queryLastCompletedTimeline()
    {
        _lastCompletedCachedTimeline = _lastCompletedTimeline.load();
        return _lastCompletedCachedTimeline;
    }

    void VkmGpuEventTimelineWebGPU::waitIdle(const uint64_t timeoutMs)
    {
        // WebGPU has no synchronous fence-wait primitive on Web; this is a bounded best-effort
        // poll driven by the browser event loop via emscripten_sleep (-sASYNCIFY), not a true block.
        const uint64_t target = _lastAllocatedTimeline;
        const double startTime = emscripten_get_now();
        while (_lastCompletedTimeline.load() < target)
        {
            if (emscripten_get_now() - startTime >= static_cast<double>(timeoutMs))
            {
                break;
            }
            emscripten_sleep(1);
        }
        _lastCompletedCachedTimeline = _lastCompletedTimeline.load();
    }

    void VkmGpuEventTimelineWebGPU::onSubmittedWorkDone(uint64_t timelineValue)
    {
        uint64_t prev = _lastCompletedTimeline.load();
        while (prev < timelineValue && !_lastCompletedTimeline.compare_exchange_weak(prev, timelineValue))
        {
        }
    }

    // VkmCommandQueueWebGPU

    VkmCommandQueueWebGPU::VkmCommandQueueWebGPU(VkmDriverBase* driver)
        : VkmCommandQueueBase(driver)
    {
    }

    VkmCommandQueueWebGPU::~VkmCommandQueueWebGPU()
    {
    }

    bool VkmCommandQueueWebGPU::initializeInner()
    {
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        _wgpuQueue = driverWebGPU->getQueue();

        _commandBufferPool = std::make_unique<VkmCommandBufferPoolWebGPU>(_driver, this);
        _gpuEventTimeline  = std::make_unique<VkmGpuEventTimelineWebGPU>(_driver);
        return true;
    }

    VkmGpuEventTimelineObject VkmCommandQueueWebGPU::submit(const CommandSubmitInfo& submitInfos)
    {
        VkmGpuEventTimelineObject timelineObject = _gpuEventTimeline->allocateGpuEventTimelineObject();
        VkmGpuEventTimelineWebGPU* timeline = static_cast<VkmGpuEventTimelineWebGPU*>(_gpuEventTimeline.get());

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        _driver->getGpuCrashHandler()->recordSubmission(this, submitInfos, timelineObject);
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        std::vector<WGPUCommandBuffer> wgpuCommandBuffers;
        wgpuCommandBuffers.reserve(submitInfos.commandBufferCount);
        for (uint32_t i = 0; i < submitInfos.commandBufferCount; ++i)
        {
            VkmCommandBufferWebGPU* cmdBuffer = static_cast<VkmCommandBufferWebGPU*>(submitInfos.commandBuffers[i]);
            WGPUCommandEncoder encoder = cmdBuffer->getWGPUCommandEncoder();
            wgpuCommandBuffers.push_back(wgpuCommandEncoderFinish(encoder, nullptr));
            wgpuCommandEncoderRelease(encoder);
        }

        wgpuQueueSubmit(_wgpuQueue, wgpuCommandBuffers.size(), wgpuCommandBuffers.data());

        for (WGPUCommandBuffer wgpuCmdBuffer : wgpuCommandBuffers)
        {
            wgpuCommandBufferRelease(wgpuCmdBuffer);
        }

        SubmitWorkDoneContext* context = new SubmitWorkDoneContext{timeline, timelineObject._timelineValue};
        const WGPUQueueWorkDoneCallbackInfo workDoneCallbackInfo{
            .mode      = WGPUCallbackMode_AllowSpontaneous,
            .callback  = onQueueWorkDone,
            .userdata1 = context,
        };
        wgpuQueueOnSubmittedWorkDone(_wgpuQueue, workDoneCallbackInfo);

        return timelineObject;
    }

    void VkmCommandQueueWebGPU::setDebugName(const char* name)
    {
        if (_wgpuQueue != nullptr)
        {
            wgpuQueueSetLabel(_wgpuQueue, toWGPUStringView(name));
        }
    }
} // namespace vkm
