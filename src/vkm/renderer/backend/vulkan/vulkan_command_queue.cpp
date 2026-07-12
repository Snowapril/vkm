// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
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
        vkCreateCommandPool(driverVulkan->getDevice(), &poolCreateInfo, nullptr, &_vkCommandPool);
    }

    VkmCommandBufferPoolVulkan::~VkmCommandBufferPoolVulkan()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        if (_vkCommandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(driverVulkan->getDevice(), _vkCommandPool, nullptr);
            _vkCommandPool = VK_NULL_HANDLE;
        }
    }

    VkmCommandBufferBase* VkmCommandBufferPoolVulkan::newCommandBuffer()
    {
        return new VkmCommandBufferVulkan(_driver, _commandQueue, this);
    }

    VKM_COMMAND_BUFFER_HANDLE VkmCommandBufferPoolVulkan::getOrCreateRHICommandBuffer()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        const VkCommandBufferAllocateInfo allocInfo{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = _vkCommandPool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer vkCommandBuffer{VK_NULL_HANDLE};
        vkAllocateCommandBuffers(driverVulkan->getDevice(), &allocInfo, &vkCommandBuffer);

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(vkCommandBuffer, &beginInfo);

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
        vkCreateSemaphore(driverVulkan->getDevice(), &semaphoreCreateInfo, nullptr, &_timelineSemaphore);
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
        vkGetSemaphoreCounterValue(driverVulkan->getDevice(), _timelineSemaphore, &value);
        _lastCompletedCachedTimeline = value;
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
            .pValues        = &_lastAllocatedTimeline,
        };
        vkWaitSemaphores(driverVulkan->getDevice(), &waitInfo, timeoutNs);
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

        _driver->getGpuCrashHandler()->recordSubmission(this, submitInfos, timelineObject);

        std::vector<VkCommandBufferSubmitInfo> cmdBufferInfos;
        cmdBufferInfos.reserve(submitInfos.commandBufferCount);
        for (uint32_t i = 0; i < submitInfos.commandBufferCount; ++i)
        {
            VkmCommandBufferVulkan* cmdBuffer = static_cast<VkmCommandBufferVulkan*>(submitInfos.commandBuffers[i]);
            VkCommandBuffer vkCmd = cmdBuffer->getVkCommandBuffer();
            vkEndCommandBuffer(vkCmd);
            cmdBufferInfos.push_back(VkCommandBufferSubmitInfo{
                .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = vkCmd,
            });
        }

        const uint64_t signalValue = timelineObject._timelineValue;
        const VkSemaphoreSubmitInfo signalSemaphoreInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = timeline->getTimelineSemaphore(),
            .value     = signalValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };

        const VkSubmitInfo2 submitInfo2{
            .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount   = (uint32_t)cmdBufferInfos.size(),
            .pCommandBufferInfos      = cmdBufferInfos.data(),
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos    = &signalSemaphoreInfo,
        };
        VKM_VK_CHECK_RESULT_MSG(vkQueueSubmit2(_vkQueue, 1, &submitInfo2, VK_NULL_HANDLE), "Failed to submit command buffer(s) to graphics queue");

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
        vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo);
#else
        (void)name;
#endif
    }
} // namespace vkm
