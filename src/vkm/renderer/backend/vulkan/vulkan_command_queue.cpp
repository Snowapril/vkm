// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/vulkan/vulkan_swapchain.h>
#include <vkm/renderer/backend/common/gpu_crash_handler.h>

#include <volk.h>

namespace vkm
{
    // VkmCommandBufferPoolVulkan

    VkmCommandBufferPoolVulkan::VkmCommandBufferPoolVulkan(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue)
        : VkmCommandBufferPoolBase(driver, commandQueue)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(driver);
        uint32_t queueFamilyIndex = driverVulkan->getQueueFamilyIndex(commandQueue->getQueueType());

        const VkCommandPoolCreateInfo poolCreateInfo{
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamilyIndex,
        };
        // Nothing downstream can proceed without a pool: every command buffer this queue ever
        // records is allocated from it.
        VKM_VK_ASSERT(vkCreateCommandPool(driverVulkan->getDevice(), &poolCreateInfo, nullptr, &_vkCommandPool),
            "Failed to create command pool");
    }

    VkmCommandBufferPoolVulkan::~VkmCommandBufferPoolVulkan()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        // Destroying the pool frees every command buffer allocated from it, whether it is
        // awaiting completion or parked for reuse -- no separate free pass needed.
        _retiredCommandBuffers.clear();
        _availableCommandBuffers.clear();
        if (_vkCommandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(driverVulkan->getDevice(), _vkCommandPool, nullptr);
            _vkCommandPool = VK_NULL_HANDLE;
        }
    }

    void VkmCommandBufferPoolVulkan::retireRHICommandBuffer(VkCommandBuffer commandBuffer,
                                                            const VkmGpuEventTimelineObject& timelineObject)
    {
        if (commandBuffer == VK_NULL_HANDLE)
        {
            return;
        }
        _retiredCommandBuffers.emplace_back(commandBuffer, timelineObject);
    }

    VkmCommandBufferBase* VkmCommandBufferPoolVulkan::newCommandBuffer()
    {
        return new VkmCommandBufferVulkan(_driver, _commandQueue, this);
    }

    VKM_COMMAND_BUFFER_HANDLE VkmCommandBufferPoolVulkan::getOrCreateRHICommandBuffer()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        // Anything whose submission has completed is no longer pending and may be recorded
        // again. Polling here rather than waiting keeps this off the critical path:
        // queryLastCompletedTimeline() is a plain vkGetSemaphoreCounterValue, and whatever is
        // still pending is simply reconsidered on the next acquire. Timeline values are
        // monotonic, so an object that was allocated but never submitted is still overtaken by
        // a later submission.
        for (auto it = _retiredCommandBuffers.begin(); it != _retiredCommandBuffers.end();)
        {
            VkmGpuEventTimelineBase* timeline = it->second._gpuEventTimeline;
            if (timeline != nullptr && timeline->queryLastCompletedTimeline() >= it->second._timelineValue)
            {
                _availableCommandBuffers.push_back(it->first);
                it = _retiredCommandBuffers.erase(it);
            }
            else
            {
                ++it;
            }
        }

        VkCommandBuffer vkCommandBuffer{VK_NULL_HANDLE};
        if (_availableCommandBuffers.empty())
        {
            const VkCommandBufferAllocateInfo allocInfo{
                .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool        = _vkCommandPool,
                .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            if (!VKM_VK_CHECK_RESULT_MSG(vkAllocateCommandBuffers(driverVulkan->getDevice(), &allocInfo, &vkCommandBuffer),
                    "Failed to allocate command buffer"))
            {
                return static_cast<VKM_COMMAND_BUFFER_HANDLE>(nullptr);
            }
        }
        else
        {
            // Reused as-is: the pool carries VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            // so vkBeginCommandBuffer below implicitly resets a buffer in the executable state.
            // An explicit vkResetCommandBuffer would only repeat that work.
            vkCommandBuffer = _availableCommandBuffers.back();
            _availableCommandBuffers.pop_back();
        }

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        // A buffer that never entered the recording state cannot be recorded into or submitted, so
        // hand back a null handle rather than one that will fail at every later vkCmd* call.
        if (!VKM_VK_CHECK_RESULT_MSG(vkBeginCommandBuffer(vkCommandBuffer, &beginInfo),
                "Failed to begin command buffer recording"))
        {
            _availableCommandBuffers.push_back(vkCommandBuffer);
            return static_cast<VKM_COMMAND_BUFFER_HANDLE>(nullptr);
        }

        return static_cast<VKM_COMMAND_BUFFER_HANDLE>(vkCommandBuffer);
    }

    // VkmGpuEventTimelineVulkan

    VkmGpuEventTimelineVulkan::VkmGpuEventTimelineVulkan(VkmDriverBase* driver)
        : VkmGpuEventTimelineBase(driver)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(driver);

        const VkSemaphoreTypeCreateInfo timelineCreateInfo{
            .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue  = 0,
        };
        const VkSemaphoreCreateInfo semaphoreCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timelineCreateInfo,
        };
        // Without the timeline semaphore every wait and every completion query below is
        // meaningless, and retired command buffers would be recycled while still in flight.
        VKM_VK_ASSERT(vkCreateSemaphore(driverVulkan->getDevice(), &semaphoreCreateInfo, nullptr, &_timelineSemaphore),
            "Failed to create timeline semaphore");
    }

    VkmGpuEventTimelineVulkan::~VkmGpuEventTimelineVulkan()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        if (_timelineSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(driverVulkan->getDevice(), _timelineSemaphore, nullptr);
            _timelineSemaphore = VK_NULL_HANDLE;
        }
    }

    uint64_t VkmGpuEventTimelineVulkan::queryLastCompletedTimeline()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        uint64_t value = 0;
        // On failure `value` is untouched, so publishing it would report "nothing completed" and
        // let callers recycle command buffers the GPU is still reading. Keep the last known value.
        if (VKM_VK_CHECK_RESULT_MSG(vkGetSemaphoreCounterValue(driverVulkan->getDevice(), _timelineSemaphore, &value),
                "Failed to query timeline semaphore counter"))
        {
            _lastCompletedCachedTimeline = value;
        }
        return _lastCompletedCachedTimeline;
    }

    void VkmGpuEventTimelineVulkan::waitIdle(const uint64_t timeoutMs)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const uint64_t timeoutNs = timeoutMs * 1000000ULL;
        const VkSemaphoreWaitInfo waitInfo{
            .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores    = &_timelineSemaphore,
            .pValues        = &_lastSubmittedTimeline,
        };
        // The result is checked because VK_TIMEOUT here is silent otherwise, and every caller of
        // waitIdle() goes on to destroy resources on the assumption the GPU is done with them.
        const VkResult result = vkWaitSemaphores(driverVulkan->getDevice(), &waitInfo, timeoutNs);
        if (result != VK_SUCCESS)
        {
            VKM_DEBUG_ERROR(fmt::format("Timed out waiting for timeline value {} (last completed {})",
                                        _lastSubmittedTimeline, queryLastCompletedTimeline())
                                .c_str());
        }
    }

    // VkmCommandQueueVulkan

    VkmCommandQueueVulkan::VkmCommandQueueVulkan(VkmDriverBase* driver)
        : VkmCommandQueueBase(driver)
    {
    }

    VkmCommandQueueVulkan::~VkmCommandQueueVulkan()
    {
    }

    bool VkmCommandQueueVulkan::initializeInner()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        uint32_t queueFamilyIndex = driverVulkan->getQueueFamilyIndex(_queueType);
        vkGetDeviceQueue(driverVulkan->getDevice(), queueFamilyIndex, _queueIndex, &_vkQueue);

        _commandBufferPool = std::make_unique<VkmCommandBufferPoolVulkan>(_driver, this);
        _gpuEventTimeline  = std::make_unique<VkmGpuEventTimelineVulkan>(_driver);
        return true;
    }

    VkmGpuEventTimelineObject VkmCommandQueueVulkan::submit(const CommandSubmitInfo& submitInfos)
    {
        VkmGpuEventTimelineObject timelineObject = _gpuEventTimeline->allocateGpuEventTimelineObject();
        VkmGpuEventTimelineVulkan* timeline = static_cast<VkmGpuEventTimelineVulkan*>(_gpuEventTimeline.get());

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        _driver->getGpuCrashHandler()->recordSubmission(this, submitInfos, timelineObject);
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        std::vector<VkCommandBufferSubmitInfo> cmdBufferInfos;
        cmdBufferInfos.reserve(submitInfos.commandBufferCount);
        for (uint32_t i = 0; i < submitInfos.commandBufferCount; ++i)
        {
            VkmCommandBufferVulkan* cmdBuffer = static_cast<VkmCommandBufferVulkan*>(submitInfos.commandBuffers[i]);
            VkCommandBuffer vkCmd = cmdBuffer->getVkCommandBuffer();
            // A buffer that failed to end is not in the executable state; submitting it is a
            // validation error and can lose the device. Drop it from this batch instead.
            if (!VKM_VK_CHECK_RESULT_MSG(vkEndCommandBuffer(vkCmd), "Failed to end command buffer recording"))
            {
                continue;
            }
            cmdBufferInfos.push_back(VkCommandBufferSubmitInfo{
                .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = vkCmd,
            });
        }

        // Swapchain present synchronization: consume the acquire semaphore as a wait and the
        // per-image render-finished semaphore as an extra signal. Only the render-graph submit
        // sets presentSwapChain; side submits (e.g. uploadToBuffer) leave it null.
        VkmSwapChainVulkan* presentSwapChain = static_cast<VkmSwapChainVulkan*>(submitInfos.presentSwapChain);

        uint32_t waitSemaphoreCount = 0;
        VkSemaphoreSubmitInfo waitSemaphoreInfo{};
        if (presentSwapChain != nullptr)
        {
            VkSemaphore acquireSemaphore = presentSwapChain->takePendingAcquireSemaphore();
            if (acquireSemaphore != VK_NULL_HANDLE)
            {
                waitSemaphoreInfo = VkSemaphoreSubmitInfo{
                    .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = acquireSemaphore,
                    // Conservative: the first access of the acquired image is an in-command-buffer
                    // layout transition, so wait before any command stage runs.
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                };
                waitSemaphoreCount = 1;
            }
        }

        const uint64_t signalValue = timelineObject._timelineValue;
        std::array<VkSemaphoreSubmitInfo, 2> signalSemaphoreInfos{};
        signalSemaphoreInfos[0] = VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = timeline->getTimelineSemaphore(),
            .value     = signalValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        uint32_t signalSemaphoreCount = 1;
        if (presentSwapChain != nullptr)
        {
            const VkSemaphore renderFinishedSemaphore = presentSwapChain->takeRenderFinishedSemaphoreForSignal();
            if (renderFinishedSemaphore != VK_NULL_HANDLE)
            {
                signalSemaphoreInfos[1] = VkSemaphoreSubmitInfo{
                    .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = renderFinishedSemaphore,
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                };
                signalSemaphoreCount = 2;
            }
        }

        const VkSubmitInfo2 submitInfo2{
            .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount   = waitSemaphoreCount,
            .pWaitSemaphoreInfos      = &waitSemaphoreInfo,
            .commandBufferInfoCount   = (uint32_t)cmdBufferInfos.size(),
            .pCommandBufferInfos      = cmdBufferInfos.data(),
            .signalSemaphoreInfoCount = signalSemaphoreCount,
            .pSignalSemaphoreInfos    = signalSemaphoreInfos.data(),
        };
        VKM_VK_CHECK_RESULT_MSG(vkQueueSubmit2(_vkQueue, 1, &submitInfo2, VK_NULL_HANDLE), "Failed to submit command buffer(s) to graphics queue");

        timeline->markTimelineSubmitted(signalValue);
        return timelineObject;
    }

    void VkmCommandQueueVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_QUEUE,
            .objectHandle = reinterpret_cast<uint64_t>(_vkQueue),
            .pObjectName  = name,
        };
        VKM_VK_CHECK_RESULT_MSG(vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo),
            "Failed to set debug name on command queue");
#else
        (void)name;
#endif
    }
} // namespace vkm
