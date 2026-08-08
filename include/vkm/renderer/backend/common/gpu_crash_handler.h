// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

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
    class VkmStagingBuffer;
    struct CommandSubmitInfo;
    struct VkmGpuEventTimelineObject;

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
    // Fixed cap on subgraphs tracked per frame for completion-marker purposes; subgraph count
    // (VkmRenderGraph::_currentSubGraphId) is otherwise unbounded. Subgraphs at or beyond this
    // index are skipped for marker-writing (a logged warning, not an assert/resize).
    static constexpr uint32_t MAX_SUBGRAPHS_PER_FRAME = 128;

    /*
    * @brief One recorded command-buffer submission, kept only while
    * VkmDriverBase::isGpuCrashDumpEnabled() is true.
    * @details A bounded ring entry VkmGpuCrashHandler::reportCrash() uses to classify recent
    * submissions as COMPLETED or SUSPECT, and to report GPU-verified per-subgraph completion.
    */
    struct VkmGpuSubmissionBreadcrumb
    {
        uint64_t timelineValue = 0;
        VkmGpuEventTimelineBase* timeline = nullptr;  // owning queue's timeline
        std::string queueName;
        std::vector<std::string> commandBufferNames;
        uint32_t frameIndex = 0;
        // Subgraphs recorded into this submission's command buffers; see
        // VkmCommandBufferBase::writeCompletionMarker.
        std::vector<uint32_t> subGraphIds;
    };
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    /*
    * @brief Backend-agnostic GPU crash/device-lost reporter, owned one per driver by VkmDriverBase.
    * @details Each backend calls recordSubmission() from its VkmCommandQueueBase::submit() override,
    * and reportCrash() once it has detected a device-lost/GPU-error condition: Vulkan
    * VK_ERROR_DEVICE_LOST, Metal4 MTL4CommitFeedback.error, WebGPU's device-lost callback.
    * With VKM_ENABLE_GPU_BREAD_CRUMBS (CMake GPU_BREAD_CRUMBS, on by default outside Release), it
    * also owns the breadcrumb ring and the per-subgraph completion-marker buffer of
    * FRAME_COUNT * MAX_SUBGRAPHS_PER_FRAME uint32_t slots. VkmRenderGraph::execute() writes a
    * literal `1` into a subgraph's slot as that subgraph's last GPU command, and reportCrash() reads
    * the buffer back to tell per subgraph whether its commands finished -- which the whole-submission
    * SUSPECT verdict cannot, not distinguishing "never started" from "started but never finished".
    * Without the macro, a crash is still always reported, just without submission history.
    */
    class VkmGpuCrashHandler
    {
    public:
        explicit VkmGpuCrashHandler(VkmDriverBase* driver);

        /*
        * @brief Logs a GPU crash report.
        * @details Backend name, error code and reason are always logged, regardless of
        * enableGpuCrashDump or VKM_ENABLE_GPU_BREAD_CRUMBS. With the macro defined and breadcrumbs
        * recorded, walks them newest-first and classifies each as COMPLETED (its timeline value was
        * already reached) or SUSPECT (may be the faulting submission, or simply still in flight),
        * then, if the marker buffer is readable, each of its subgraphs as COMPLETED or NOT
        * COMPLETED.
        * @param backendName Backend reporting the crash.
        * @param errorCode Native error code.
        * @param reason Human-readable reason.
        */
        void reportCrash(const char* backendName, const std::string& errorCode, const std::string& reason);

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        /*
        * @brief Records a submission into the bounded breadcrumb ring.
        * @details Stores the queue name, command-buffer debug names, frame index and recorded
        * subgraph IDs. A no-op unless the driver was launched with enableGpuCrashDump. The ring is
        * capped at MAX_BREADCRUMB_ENTRIES, oldest evicted first.
        * @param queue Queue the submission went to.
        * @param submitInfo Command buffers and frame index of the submission.
        * @param timelineObject Timeline value the submission signals.
        */
        void recordSubmission(VkmCommandQueueBase* queue, const CommandSubmitInfo& submitInfo, VkmGpuEventTimelineObject timelineObject);

        /*
        * @brief Handle to the persistent completion-marker buffer, created on first use.
        * @return The buffer, or VKM_INVALID_RESOURCE_HANDLE unless isGpuCrashDumpEnabled().
        */
        VkmResourceHandle getOrCreateMarkerBuffer();

        /*
        * @brief Handle to a small persistent buffer holding a single uint32_t value of `1`, the
        * copy source for VkmCommandBufferBase::writeCompletionMarker().
        * @details Created alongside the marker buffer.
        * @return The buffer, or VKM_INVALID_RESOURCE_HANDLE unless isGpuCrashDumpEnabled().
        */
        VkmResourceHandle getOrCreateOneBuffer();

        /*
        * @brief Byte offset within the marker buffer for one subgraph of one frame, for use with
        * writeCompletionMarker().
        * @param frameIndex Frame slot.
        * @param subGraphId Subgraph within that frame.
        * @return The offset, or INVALID_VALUE32 with a logged warning when subGraphId is at or
        * beyond MAX_SUBGRAPHS_PER_FRAME.
        */
        uint32_t getMarkerOffset(uint32_t frameIndex, uint32_t subGraphId) const;

        /*
        * @brief Blocks until a frame's prior GPU work has finished, then zeroes its marker slice.
        * @details Must be called before that frame's subgraphs are re-recorded. A no-op unless
        * isGpuCrashDumpEnabled().
        * @param frameIndex Frame slot to clear.
        */
        void clearFrameMarkers(uint32_t frameIndex);
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    private:
        VkmDriverBase* _driver;

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        void ensureMarkerBuffersCreated();

        std::mutex _mutex;
        std::deque<VkmGpuSubmissionBreadcrumb> _breadcrumbs;

        VkmStagingBuffer* _markerBuffer = nullptr;
        VkmStagingBuffer* _oneBuffer = nullptr;

        static constexpr size_t MAX_BREADCRUMB_ENTRIES = 64;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
    };
}
