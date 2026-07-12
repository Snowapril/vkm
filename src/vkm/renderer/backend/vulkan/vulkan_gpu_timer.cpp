// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_gpu_timer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/base/common.h>

#include <array>

namespace vkm
{
    VkmGpuTimerVulkan::VkmGpuTimerVulkan(VkmDriverVulkan* driver)
        : _driver(driver)
    {
    }

    VkmGpuTimerVulkan::~VkmGpuTimerVulkan()
    {
    }

    bool VkmGpuTimerVulkan::initialize()
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_driver->getPhysicalDevice(), &properties);
        if (properties.limits.timestampComputeAndGraphics == VK_FALSE)
        {
            VKM_DEBUG_INFO("GPU timestamp queries are not supported on this device; GPU frame time will report 0");
            _supported = false;
            return true;
        }
        _timestampPeriodNs = properties.limits.timestampPeriod;

        const VkQueryPoolCreateInfo queryPoolCreateInfo{
            .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType  = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2 * FRAME_COUNT,
        };
        const VkResult vkResult = vkCreateQueryPool(_driver->getDevice(), &queryPoolCreateInfo, nullptr, &_queryPool);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create GPU timer query pool"))
        {
            return false;
        }

        _supported = true;
        return true;
    }

    void VkmGpuTimerVulkan::destroy()
    {
        if (_queryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(_driver->getDevice(), _queryPool, nullptr);
            _queryPool = VK_NULL_HANDLE;
        }
    }

    void VkmGpuTimerVulkan::writeBeginTimestamp(VkCommandBuffer commandBuffer)
    {
        if (!_supported)
        {
            return;
        }
        _currentSlot = (_currentSlot + 1) % FRAME_COUNT;
        vkCmdResetQueryPool(commandBuffer, _queryPool, _currentSlot * 2, 2);
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, _queryPool, _currentSlot * 2);
        ++_framesRecorded;
    }

    void VkmGpuTimerVulkan::writeEndTimestamp(VkCommandBuffer commandBuffer)
    {
        if (!_supported)
        {
            return;
        }
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, _queryPool, _currentSlot * 2 + 1);
    }

    double VkmGpuTimerVulkan::getLastGpuFrameTimeMs()
    {
        if (!_supported || _framesRecorded <= FRAME_COUNT)
        {
            // The ring hasn't cycled once yet, so the "oldest" slot below hasn't actually
            // been reset+written -- reading it would trip
            // VUID-vkGetQueryPoolResults-None-09401 ("query not reset").
            return _lastFrameTimeMs;
        }

        // Read the oldest slot in the ring (about to be overwritten by the next
        // writeBeginTimestamp call) rather than the one just recorded, so the GPU has had
        // FRAME_COUNT-1 frames worth of time to actually finish it -- avoids stalling here.
        const uint32_t readSlot = (_currentSlot + 1) % FRAME_COUNT;

        std::array<uint64_t, 2> timestamps{};
        const VkResult vkResult = vkGetQueryPoolResults(_driver->getDevice(), _queryPool, readSlot * 2, 2,
            sizeof(timestamps), timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (vkResult == VK_SUCCESS)
        {
            const uint64_t deltaTicks = timestamps[1] - timestamps[0];
            _lastFrameTimeMs = static_cast<double>(deltaTicks) * static_cast<double>(_timestampPeriodNs) / 1'000'000.0;
        }
        // VK_NOT_READY (query not yet completed) or VK_ERROR_* (e.g. queried before any
        // write) both fall through to returning the last-known-good value.
        return _lastFrameTimeMs;
    }
} // namespace vkm
