// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmCommandQueueBase;
    class VkmDriverBase;

    /*
    * @brief One GPU-timed span that executed on one command queue.
    * @details Timestamps are nanoseconds in the GPU's own clock domain, not correlated to the CPU
    * clock VkmProfileZone uses, so a GPU and a CPU trace cannot be overlaid on one timeline. The
    * viewer only ever shows offsets from the frame's own start.
    */
    struct VkmGpuProfileZone
    {
        // Not owned. Interned via VkmCpuProfiler::internName() by whoever opened the zone, so it
        // outlives the frame ring.
        const char* _name = nullptr;
        uint64_t _beginNs = 0;
        uint64_t _endNs = 0;
        uint16_t _depth = 0;  // 0 = the submission-wide zone, 1 = the subgraphs inside it
        // The VkmRenderSubGraph this zone timed, or INVALID_VALUE32 for the depth-0 zone.
        uint32_t _subGraphId = INVALID_VALUE32;

        inline uint64_t getDurationNs() const { return (_endNs > _beginNs) ? (_endNs - _beginNs) : 0; }
    };

    // One command queue's zones within a single collected frame, sorted by begin time
    // (parent first), which is the order the chart walks.
    struct VkmGpuQueueTimeline
    {
        VkmCommandQueueType _queueType = VkmCommandQueueType::Graphics;
        uint32_t _queueIndex = 0;
        std::string _queueName; // VkmCommandQueueBase::getQueueName(), e.g. "MainGraphics"
        std::vector<VkmGpuProfileZone> _zones;
        // True when a submission asked to time more zones than kMaxZonesPerSubmission, or when
        // no slot bucket was free; the UI surfaces it rather than showing a silently short frame.
        bool _overflowed = false;
    };

    /*
    * @brief Every GPU zone that completed and was attributed to one engine frame.
    * @details _beginNs/_endNs span the union of every zone in the frame across every queue,
    * so a queue whose work does not line up with the graphics queue is not clipped out.
    */
    struct VkmGpuProfileFrame
    {
        uint32_t _frameNumber = 0;
        uint64_t _beginNs = 0;
        uint64_t _endNs = 0;
        std::vector<VkmGpuQueueTimeline> _queues;

        inline uint64_t getDurationNs() const { return (_endNs > _beginNs) ? (_endNs - _beginNs) : 0; }
    };

    // Cheap per-frame row for the history strip, so the UI does not deep-copy the whole ring
    // every time it draws.
    struct VkmGpuProfileFrameSummary
    {
        uint32_t _frameNumber = 0;
        uint64_t _durationNs = 0;
    };

    // How long one zone name was executing inside a queried time range, and how many zones
    // contributed. `_name` is borrowed from the zones it came from, so it lives as long as they do.
    struct VkmGpuProfileZoneTotal
    {
        const char* _name = nullptr;
        uint64_t _totalNs = 0;
        uint32_t _count = 0;
    };

    /*
    * @brief Totals how long each zone was executing inside a time range, longest first.
    * @details The GPU counterpart of vkmAggregateProfileRange, identical in behaviour: zones are
    * clipped to the range rather than counted whole, every depth contributes its own overlap (so a
    * submission zone and the subgraphs inside it are all counted and the totals sum to more than
    * the range), and grouping is by name text rather than by pointer.
    * @param frame Frame whose zones are totalled.
    * @param beginNs Start of the range.
    * @param endNs End of the range.
    * @return One entry per zone name, longest first.
    */
    std::vector<VkmGpuProfileZoneTotal> vkmAggregateGpuProfileRange(const VkmGpuProfileFrame& frame,
                                                                   uint64_t beginNs, uint64_t endNs);

    /*
    * @brief Sliding-window mean GPU duration per render graph subgraph.
    * @details Keyed by subgraph name rather than by VkmRenderSubGraph::getSubGraphId(): ids restart
    * at 0 for every graph, so two windows submitting in one frame reuse them and a graph that
    * changes shape moves a pass onto another id. Zones sharing a name are averaged together.
    * A type of its own rather than members on VkmGpuProfiler so it is exercisable from zones built
    * by hand, the same reason vkmAggregateGpuProfileRange is a free function.
    */
    class VkmGpuSubGraphAverages
    {
    public:
        /*
        * @brief Adds one submission's zones to the windows.
        * @param zones Zones of one submission. Only depth-1 zones naming a subgraph contribute;
        * the submission-wide zone and unnamed zones are ignored.
        */
        void addZones(const std::vector<VkmGpuProfileZone>& zones);

        /*
        * @brief Mean duration of the samples currently in a subgraph's window, in milliseconds.
        * @param subGraphName Name its zones were opened with.
        * @return 0 when nothing has been recorded under that name.
        */
        double getAverageMs(const std::string& subGraphName) const;

        /*
        * @brief Samples currently in a subgraph's window, at most kWindowSize.
        * @param subGraphName Name its zones were opened with.
        * @return 0 when nothing has been recorded under that name.
        */
        uint32_t getSampleCount(const std::string& subGraphName) const;

        void clear();

        // ~1 second at 60 fps.
        static constexpr size_t kWindowSize = 64;

    private:
        struct Window
        {
            std::array<uint64_t, kWindowSize> _samples{};
            size_t _next = 0;
            uint32_t _count = 0;
            uint64_t _sumNs = 0;
        };

        std::unordered_map<std::string, Window> _windows;
    };

    /*
    * @brief Writes frames in Chrome Trace Event Format, loadable in chrome://tracing or
    * ui.perfetto.dev.
    * @details One complete event ("ph":"X") per zone plus one "thread_name" metadata event
    * ("ph":"M") per command queue. Timestamps are emitted in MICROSECONDS as the format requires,
    * converted from the nanoseconds VkmGpuProfileZone stores. Uses a different pid than the CPU
    * export so both files can be loaded into one viewer without their rows colliding. A free
    * function so the format is exercisable from frames built by hand.
    * @param frames Frames to write.
    * @param path Destination file.
    * @return False if the file cannot be written, and unconditionally when the CHROME_TRACING
    * CMake option is off.
    */
    bool vkmWriteGpuChromeTrace(const std::vector<VkmGpuProfileFrame>& frames, const std::string& path);

    /*
    * @brief Collects per-subgraph GPU execution times, grouped by the command queue they ran on,
    * and keeps a ring of the most recent frames for live inspection.
    * @details Driven by VkmRenderGraph::execute(), which brackets the whole submission in a depth-0
    * zone and each subgraph in a depth-1 zone; collect() is called once per frame by
    * VkmEngine::loopInner(). The ImGui front end is VkmGpuProfilerInspector, a separate type so
    * this half stays ImGui-free and unit-testable.
    * Owned by VkmDriverBase rather than being a singleton like VkmCpuProfiler, because it owns
    * backend query resources whose lifetime is the device's.
    * Recording is always on where the backend supports timestamps, which keeps
    * getLastFrameGpuTimeMs() alive with the inspector closed; isCapturing() only gates whether
    * resolved frames are kept in the history ring.
    * Not thread-safe and unlocked: every entry point is called from the thread driving the frame
    * loop.
    */
    class VkmGpuProfiler
    {
    public:
        explicit VkmGpuProfiler(VkmDriverBase* driver);
        ~VkmGpuProfiler();

        VkmGpuProfiler(const VkmGpuProfiler&) = delete;
        VkmGpuProfiler& operator=(const VkmGpuProfiler&) = delete;

        /*
        * @brief Asks the driver for a timestamp pool.
        * @return True even when the backend has no timestamp support -- that is a capability, not a
        * failure. isSupported() then reports false and every recording entry point becomes a no-op.
        */
        bool initialize();
        void destroy();

        inline bool isSupported() const { return _supported; }

        /*
        * @brief Opens a timed submission on a queue.
        * @details Must be called after beginCommandBuffer() and outside any render pass: it records
        * the backend's reset for exactly the 2*zoneCount slots the submission will write, which
        * Vulkan requires before any of them can be read back.
        * @param queue Queue the submission runs on.
        * @param commandBuffer Command buffer the reset is recorded into.
        * @param zoneCount Zones the submission will time.
        * @return The submission id, or kInvalidSubmission when unsupported or when no slot bucket
        * is free, in which case every zone call below no-ops.
        */
        uint32_t beginSubmission(VkmCommandQueueBase* queue, VkmCommandBufferBase* commandBuffer,
                                 uint32_t zoneCount);

        /*
        * @brief Opens a zone. Zones nest.
        * @param commandBuffer Command buffer the zone is recorded into.
        * @param submission Submission id from beginSubmission().
        * @param name Zone name. Must outlive the frame ring: a string literal or a pointer from
        * VkmCpuProfiler::internName().
        * @param subGraphId Subgraph this zone times, or INVALID_VALUE32.
        * @param depth Nesting depth, which the chart lays rows out by.
        */
        void beginZone(VkmCommandBufferBase* commandBuffer, uint32_t submission, const char* name,
                       uint32_t subGraphId, uint16_t depth);

        /*
        * @brief Closes the innermost open zone.
        * @details Closing the outermost one also ends the submission's recording, giving the
        * backend its chance to record a resolve into the same command buffer, so it must happen
        * before endCommandBuffer().
        * @param commandBuffer Command buffer the zone was recorded into.
        * @param submission Submission id from beginSubmission().
        */
        void endZone(VkmCommandBufferBase* commandBuffer, uint32_t submission);

        /*
        * @brief Hands over the timeline the submission was submitted with. Call right after submit().
        * @details collect() will not read a submission's timestamps until that timeline completes.
        * @param submission Submission id from beginSubmission().
        * @param timeline Timeline the submission signals.
        */
        void endSubmission(uint32_t submission, const VkmGpuEventTimelineObject& timeline);

        /*
        * @brief Resolves every submission the GPU has finished and advances the frame number new
        * submissions are stamped with. Call once per frame from the frame loop.
        * @details Resolved frames are appended to the ring only while capturing. Never blocks: a
        * submission whose timeline has not completed is left for a later call.
        */
        void collect();

        /*
        * @brief Starts or stops keeping resolved frames in the history ring.
        * @details Turning capture on drops the frames already collected, so a capture never starts
        * with the tail of the last one. Submissions already in flight still land in the ring.
        * @param capturing Whether to keep resolved frames.
        */
        void setCapturing(bool capturing);
        inline bool isCapturing() const { return _capturing; }

        // Number of collected frames currently in the ring.
        size_t getFrameCount() const;

        // Frame number + duration for every frame in the ring, oldest first.
        std::vector<VkmGpuProfileFrameSummary> copyFrameSummaries() const;

        /*
        * @brief Deep copy of one collected frame.
        * @param index Ring index, 0 = oldest.
        * @param outFrame Receives the frame.
        * @return False when `index` is out of range.
        */
        bool copyFrame(size_t index, VkmGpuProfileFrame& outFrame) const;

        /*
        * @brief Drops every collected frame.
        * @details Unlike VkmCpuProfiler::clear(), frame numbering is NOT restarted: submissions
        * recorded before the clear are still in flight and already carry their numbers, and
        * reusing those numbers would merge them into unrelated frames.
        */
        void clear();

        /*
        * @brief GPU time the most recently resolved submission spanned, in milliseconds.
        * @details The union of its zones rather than its depth-0 one, because WebGPU cannot time a
        * zone that encloses no pass and so has no depth-0 zone. Latched on every resolve regardless
        * of isCapturing(), since it backs the always-visible debug overlay stat. 0 where the
        * backend has no timestamp support.
        */
        inline double getLastFrameGpuTimeMs() const { return _lastFrameGpuTimeMs; }

        /*
        * @brief Per-subgraph rolling averages, fed by every resolved submission.
        * @details Independent of isCapturing() and of clear(), like getLastFrameGpuTimeMs(): the
        * render graph inspector reads them with this window closed and with no capture armed.
        */
        inline const VkmGpuSubGraphAverages& getSubGraphAverages() const { return _subGraphAverages; }

        /*
        * @brief Writes every collected frame through vkmWriteGpuChromeTrace.
        * @param path Destination file.
        * @return False if the file cannot be written.
        */
        bool exportChromeTrace(const std::string& path) const;

        // ~4 seconds of history at 60 fps, matching VkmCpuProfiler::kMaxFrameHistory.
        static constexpr size_t kMaxFrameHistory = 240;
        // Zones one submission may time. Render graphs run a handful of subgraphs today, so this
        // is generous; asking for more marks the frame's timeline as overflowed.
        static constexpr uint32_t kMaxZonesPerSubmission = 32;
        // Submissions that may be in flight at once (FRAME_COUNT slots x several windows, with
        // headroom). Each owns a fixed bucket of timestamp slots, so slot allocation is a bucket
        // index rather than interval arithmetic.
        static constexpr uint32_t kMaxPendingSubmissions = 16;
        // Total timestamp slots the driver is asked for: a begin/end pair per zone per bucket.
        static constexpr uint32_t kTimestampSlotCount = 2 * kMaxZonesPerSubmission * kMaxPendingSubmissions;

        static constexpr uint32_t kInvalidSubmission = INVALID_VALUE32;

    private:
        struct PendingZone
        {
            const char* _name = nullptr;
            uint32_t _subGraphId = INVALID_VALUE32;
            uint16_t _depth = 0;
            // What VkmCommandBufferBase::endGpuZone() reported: false when the backend had
            // nowhere to put this zone's timestamps (WebGPU), so its slots hold nothing.
            bool _timed = false;
        };

        struct PendingSubmission
        {
            VkmGpuEventTimelineObject _timeline;
            uint32_t _firstSlot = 0;
            uint32_t _frameNumber = 0;
            VkmCommandQueueType _queueType = VkmCommandQueueType::Graphics;
            uint32_t _queueIndex = 0;
            std::string _queueName;
            std::vector<PendingZone> _zones;      // in begin order; zone i owns slots 2i / 2i+1
            std::vector<uint32_t> _openZoneStack; // indices into _zones
            bool _overflowed = false;
            bool _submitted = false;
        };

        // Stack entry for a zone that was opened past kMaxZonesPerSubmission and therefore has no
        // slots. It is still pushed so that open/close stay paired.
        static constexpr uint32_t kSuppressedZone = INVALID_VALUE32;

        // Moves one finished submission's resolved zones into the ring (or drops them when not
        // capturing), and always latches getLastFrameGpuTimeMs().
        void retireSubmission(PendingSubmission& submission, std::vector<VkmGpuProfileZone>&& zones);
        // Ring entry for `frameNumber`, appending a new one when the back entry is a different
        // frame. Frame numbers arrive monotonically, so no lookup by key is needed.
        VkmGpuProfileFrame& frameForNumber(uint32_t frameNumber);

        VkmDriverBase* _driver;
        bool _supported = false;
        bool _capturing = false;
        double _lastFrameGpuTimeMs = 0.0;
        double _timestampPeriodNs = 1.0;
        VkmGpuSubGraphAverages _subGraphAverages;

        // Fixed bucket ring: buckets are handed out and retired in submission order, so
        // collect() can stop at the first bucket the GPU has not finished.
        std::array<PendingSubmission, kMaxPendingSubmissions> _pending;
        uint32_t _oldestBucket = 0;
        uint32_t _pendingCount = 0;
        bool _slotExhaustionLogged = false;

        uint32_t _recordFrameNumber = 0;
        std::deque<VkmGpuProfileFrame> _frames;
    };
} // namespace vkm
