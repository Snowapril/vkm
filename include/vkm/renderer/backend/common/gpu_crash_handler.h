// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmCommandQueueBase;
    class VkmGpuEventTimelineBase;
    struct CommandSubmitInfo;
    struct VkmGpuEventTimelineObject;

    /*
    * @brief One recorded command-buffer submission, kept only while
    * VkmDriverBase::isGpuCrashDumpEnabled() is true. Bounded ring entry used by
    * VkmGpuCrashHandler::reportCrash() to classify recent submissions as COMPLETED or SUSPECT.
    */
    struct VkmGpuSubmissionBreadcrumb
    {
        uint64_t timelineValue = 0;
        VkmGpuEventTimelineBase* timeline = nullptr; // owning queue's timeline, for queryLastCompletedTimeline()
        std::string queueName;
        std::vector<std::string> commandBufferNames;
    };

    /*
    * @brief Backend-agnostic GPU crash/device-lost reporter, owned one-per-driver by
    * VkmDriverBase. Each backend calls recordSubmission() from its VkmCommandQueueBase::submit()
    * override, and reportCrash() once it has detected a device-lost/GPU-error condition
    * (Vulkan VK_ERROR_DEVICE_LOST, Metal4 MTL4CommitFeedback.error, WebGPU device-lost callback).
    */
    class VkmGpuCrashHandler
    {
    public:
        explicit VkmGpuCrashHandler(VkmDriverBase* driver);

        /*
        * @brief Record a submission's queue name and command-buffer debug names into the
        * bounded breadcrumb ring. No-op unless the driver was launched with
        * enableGpuCrashDump; the ring is capped at MAX_BREADCRUMB_ENTRIES (oldest evicted first).
        */
        void recordSubmission(VkmCommandQueueBase* queue, const CommandSubmitInfo& submitInfo, VkmGpuEventTimelineObject timelineObject);

        /*
        * @brief Log a GPU crash report: backend name, error code, and reason are always
        * logged regardless of enableGpuCrashDump. If any breadcrumbs were recorded, walks
        * them newest-first, classifying each as COMPLETED (its timeline value was already
        * reached) or SUSPECT (not yet completed -- may be the faulting submission, or simply
        * still in flight).
        */
        void reportCrash(const char* backendName, const std::string& errorCode, const std::string& reason);

    private:
        VkmDriverBase* _driver;
        std::mutex _mutex;
        std::deque<VkmGpuSubmissionBreadcrumb> _breadcrumbs;

        static constexpr size_t MAX_BREADCRUMB_ENTRIES = 64;
    };
}
