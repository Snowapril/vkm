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
    * VkmDriverBase::isGpuCrashDumpEnabled() is true. Bounded ring entry used by
    * VkmGpuCrashHandler::reportCrash() to classify recent submissions as COMPLETED or SUSPECT,
    * and (via subGraphIds + frameIndex) to report GPU-verified per-subgraph completion.
    */
    struct VkmGpuSubmissionBreadcrumb
    {
        uint64_t timelineValue = 0;
        VkmGpuEventTimelineBase* timeline = nullptr; // owning queue's timeline, for queryLastCompletedTimeline()
        std::string queueName;
        std::vector<std::string> commandBufferNames;
        uint32_t frameIndex = 0;
        std::vector<uint32_t> subGraphIds; // subgraphs recorded (see VkmCommandBufferBase::writeCompletionMarker) into this submission's command buffer(s)
    };
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    /*
    * @brief Backend-agnostic GPU crash/device-lost reporter, owned one-per-driver by
    * VkmDriverBase. Each backend calls recordSubmission() from its VkmCommandQueueBase::submit()
    * override, and reportCrash() once it has detected a device-lost/GPU-error condition
    * (Vulkan VK_ERROR_DEVICE_LOST, Metal4 MTL4CommitFeedback.error, WebGPU device-lost callback).
    *
    * When compiled with VKM_ENABLE_GPU_BREAD_CRUMBS (a CMake GPU_BREAD_CRUMBS option, ON by
    * default except in Release builds), it additionally owns the breadcrumb ring and the
    * per-subgraph GPU completion-marker buffer: a small persistent buffer of
    * FRAME_COUNT * MAX_SUBGRAPHS_PER_FRAME uint32_t slots. VkmRenderGraph::execute() writes a
    * literal `1` into a subgraph's slot as the last GPU command of that subgraph (see
    * VkmCommandBufferBase::writeCompletionMarker); reportCrash() reads the buffer back to tell,
    * per subgraph, whether its GPU commands actually finished by crash time (unlike the
    * whole-submission SUSPECT verdict above, which can't distinguish "never started" from
    * "started but never finished"). Without the macro, only detection/logging remains -- a
    * crash is always reported, just without submission history.
    */
    class VkmGpuCrashHandler
    {
    public:
        explicit VkmGpuCrashHandler(VkmDriverBase* driver);

        /*
        * @brief Log a GPU crash report: backend name, error code, and reason are always
        * logged regardless of enableGpuCrashDump or VKM_ENABLE_GPU_BREAD_CRUMBS. With the
        * macro defined and breadcrumbs recorded, walks them newest-first, classifying each
        * as COMPLETED (its timeline value was already reached) or SUSPECT (not yet
        * completed -- may be the faulting submission, or simply still in flight), then --
        * if the marker buffer is readable -- each of its recorded subgraphs as COMPLETED or
        * NOT COMPLETED per the marker buffer's contents.
        */
        void reportCrash(const char* backendName, const std::string& errorCode, const std::string& reason);

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        /*
        * @brief Record a submission's queue name, command-buffer debug names, frame index, and
        * recorded subgraph IDs into the bounded breadcrumb ring. No-op unless the driver was
        * launched with enableGpuCrashDump; the ring is capped at MAX_BREADCRUMB_ENTRIES (oldest
        * evicted first).
        */
        void recordSubmission(VkmCommandQueueBase* queue, const CommandSubmitInfo& submitInfo, VkmGpuEventTimelineObject timelineObject);

        /*
        * @brief Handle to the persistent completion-marker buffer (lazily created on first use).
        * Returns VKM_INVALID_RESOURCE_HANDLE unless isGpuCrashDumpEnabled().
        */
        VkmResourceHandle getOrCreateMarkerBuffer();

        /*
        * @brief Handle to a small persistent buffer holding a single uint32_t value of `1`,
        * used as the copy source for VkmCommandBufferBase::writeCompletionMarker(). Lazily
        * created alongside the marker buffer.
        */
        VkmResourceHandle getOrCreateOneBuffer();

        /*
        * @brief Byte offset within the marker buffer for (frameIndex, subGraphId), for use with
        * writeCompletionMarker(). Returns INVALID_VALUE32 if subGraphId >=
        * MAX_SUBGRAPHS_PER_FRAME (logs a warning).
        */
        uint32_t getMarkerOffset(uint32_t frameIndex, uint32_t subGraphId) const;

        /*
        * @brief Blocks until frameIndex's prior GPU work has finished -- no reliable wait
        * exists elsewhere in the live render loop for this (see common/AGENTS.md) -- then
        * zeroes that frame's marker slice. No-op unless isGpuCrashDumpEnabled(). Must be called
        * before a frame's subgraphs are (re)recorded.
        */
        void clearFrameMarkers(uint32_t frameIndex);
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    private:
        // maybe_unused: without VKM_ENABLE_GPU_BREAD_CRUMBS only the constructor touches this
        // field, which Clang's -Wunused-private-field rejects under -Werror.
        [[maybe_unused]] VkmDriverBase* _driver;

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
