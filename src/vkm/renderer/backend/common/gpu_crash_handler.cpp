// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/gpu_crash_handler.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>

namespace vkm
{
    VkmGpuCrashHandler::VkmGpuCrashHandler(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    void VkmGpuCrashHandler::recordSubmission(VkmCommandQueueBase* queue, const CommandSubmitInfo& submitInfo, VkmGpuEventTimelineObject timelineObject)
    {
        if (_driver->isGpuCrashDumpEnabled() == false)
        {
            return;
        }

        VkmGpuSubmissionBreadcrumb breadcrumb;
        breadcrumb.timelineValue = timelineObject._timelineValue;
        breadcrumb.timeline = timelineObject._gpuEventTimeline;
        breadcrumb.queueName = queue->getQueueName() != nullptr ? queue->getQueueName() : "<unnamed queue>";

        breadcrumb.commandBufferNames.reserve(submitInfo.commandBufferCount);
        for (uint32_t i = 0; i < submitInfo.commandBufferCount; ++i)
        {
            const std::string& debugName = submitInfo.commandBuffers[i]->getDebugName();
            breadcrumb.commandBufferNames.push_back(
                debugName.empty() ? fmt::format("{}#{}", breadcrumb.queueName, i) : debugName);
        }

        std::lock_guard<std::mutex> lock(_mutex);
        _breadcrumbs.push_back(std::move(breadcrumb));
        while (_breadcrumbs.size() > MAX_BREADCRUMB_ENTRIES)
        {
            _breadcrumbs.pop_front();
        }
    }

    void VkmGpuCrashHandler::reportCrash(const char* backendName, const std::string& errorCode, const std::string& reason)
    {
        VKM_DEBUG_ERROR(fmt::format("[GPU CRASH][{}] error={} reason={}", backendName, errorCode, reason).c_str());

        std::deque<VkmGpuSubmissionBreadcrumb> snapshot;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            snapshot = _breadcrumbs;
        }

        if (snapshot.empty())
        {
            VKM_DEBUG_ERROR("[GPU CRASH] No submission breadcrumbs recorded (launch with --enable-gpu-crash-dump to capture submission history).");
            return;
        }

        // Newest first: the most recently submitted, not-yet-completed work is the most likely
        // culprit for a fresh device-lost/GPU-error condition.
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
        {
            const VkmGpuSubmissionBreadcrumb& breadcrumb = *it;
            const bool completed = breadcrumb.timeline != nullptr
                && breadcrumb.timeline->queryLastCompletedTimeline() >= breadcrumb.timelineValue;

            std::string commandBufferList;
            for (size_t i = 0; i < breadcrumb.commandBufferNames.size(); ++i)
            {
                if (i > 0)
                {
                    commandBufferList += ", ";
                }
                commandBufferList += breadcrumb.commandBufferNames[i];
            }

            VKM_DEBUG_ERROR(fmt::format("[GPU CRASH] queue='{}' timeline={} [{}]: {}",
                breadcrumb.queueName, breadcrumb.timelineValue,
                completed ? "COMPLETED" : "SUSPECT - not yet completed", commandBufferList).c_str());
        }
    }
}
