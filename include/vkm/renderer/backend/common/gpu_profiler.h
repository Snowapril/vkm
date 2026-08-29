// Copyright (c) 2026 Snowapril

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
    * @brief How a frame's GPU timestamps were placed on the CPU clock.
    */
    enum class VkmGpuClockCorrelation : uint8_t
    {
        None,       // Nothing placed them; timestamps are raw GPU-domain offsets.
        Estimated,  // Anchored to the CPU timestamp of the submit that produced them.
        Calibrated, // Anchored to a GPU/CPU clock pair sampled from the driver.
    };

    /*
    * @brief One sampled pair of the CPU clock and the GPU timestamp counter.
    * @details `_cpuNs` is on VkmCpuProfiler::nowNs()'s clock; `_gpuTicks` is in
    * VkmDriverBase::getGpuTimestampPeriodNs() units, i.e. the units resolveGpuTimestamps()
    * reports. The two together are what map one clock onto the other.
    */
    struct VkmGpuClockCalibration
    {
        bool _valid = false;
        uint64_t _cpuNs = 0;
        uint64_t _gpuTicks = 0;
        double _timestampPeriodNs = 1.0;
    };

    /*
    * @brief Places a raw GPU timestamp on the CPU clock.
    * @details Clamps at 0 rather than wrapping: VkmCpuProfiler's epoch is taken on first use, so a
    * GPU timestamp from before it maps to a negative value.
    * @param calibration Pair anchoring the two clocks.
    * @param gpuTicks Raw GPU timestamp, in getGpuTimestampPeriodNs() units.
    * @return Nanoseconds on VkmCpuProfiler::nowNs()'s clock, or 0 when `calibration` is invalid.
    */
    uint64_t vkmGpuTicksToCpuNs(const VkmGpuClockCalibration& calibration, uint64_t gpuTicks);

    /*
    * @brief One GPU-timed span that executed on one command queue.
    * @details Timestamps are nanoseconds on VkmCpuProfiler::nowNs()'s clock -- the same one
    * VkmProfileZone uses -- so a GPU zone and the CPU scope that submitted it sit on one timeline.
    * How they got there is per frame: see VkmGpuProfileFrame::_correlation.
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

    /*
    * @brief One submit() call, on the CPU clock VkmProfileZone timestamps use.
    * @details `_beginNs`/`_endNs` bracket the call itself, so a submit that stalled reads as a
    * wide marker rather than an instant. `_gpuBeginNs` is filled in when the submission it
    * produced is resolved, and the gap from `_endNs` to it is how long the work waited on the
    * queue. A submit the profiler did not time -- a driver upload, an acceleration structure
    * build, or one made while no timestamp slot bucket was free -- reports _hasGpuWork == false
    * and has no such gap.
    */
    struct VkmGpuSubmitMarker
    {
        uint64_t _beginNs = 0;
        uint64_t _endNs = 0;
        bool _hasGpuWork = false;
        uint64_t _gpuBeginNs = 0;
    };

    // One command queue's zones within a single collected frame, sorted by begin time
    // (parent first), which is the order the chart walks.
    struct VkmGpuQueueTimeline
    {
        VkmCommandQueueType _queueType = VkmCommandQueueType::Graphics;
        uint32_t _queueIndex = 0;
        std::string _queueName; // VkmCommandQueueBase::getQueueName(), e.g. "MainGraphics"
        std::vector<VkmGpuProfileZone> _zones;
        // Every submit() made to this queue during the frame, in submit order. Includes those
        // that recorded no zones, which is the only trace an upload or a build leaves.
        std::vector<VkmGpuSubmitMarker> _submits;
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
        // Weakest correlation any submission in this frame was placed with, so a frame is only
        // reported as calibrated when every part of it is.
        VkmGpuClockCorrelation _correlation = VkmGpuClockCorrelation::None;

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
    * @brief Shifts one submission's zones so its earliest one begins where its submit returned.
    * @details The fallback for a backend with no clock calibration. Durations, gaps and ordering
    * within the submission are preserved exactly; its position relative to other submissions and
    * other queues is not measured, so the queue latency it implies is always zero.
    * @param zones Zones of one submission, in raw GPU-domain nanoseconds. Shifted in place.
    * @param submitCpuNs CPU timestamp the submission was handed to the queue at.
    */
    void vkmAnchorGpuZonesToSubmit(std::vector<VkmGpuProfileZone>& zones, uint64_t submitCpuNs);

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
        * @brief Records that the CPU handed a submission to a queue. Called by
        * VkmCommandQueueBase::submit() for every submit, timed or not.
        * @details What places GPU work against the CPU timeline it was submitted from. Unlike
        * beginSubmission(), this sees the driver's uploads and acceleration structure builds too,
        * which record no zones and would otherwise leave no trace. No-op while not capturing.
        * @param queue Queue the work was submitted to.
        * @param beginNs CPU timestamp taken before the submit, from VkmCpuProfiler::nowNs().
        * @param endNs CPU timestamp taken after it.
        * @param timeline Timeline the submission signals, which is what pairs this marker with
        * the zones the same submission recorded.
        */
        void recordSubmit(VkmCommandQueueBase* queue, uint64_t beginNs, uint64_t endNs,
                          const VkmGpuEventTimelineObject& timeline);

        /*
        * @brief How the most recently resolved frames' GPU timestamps were placed on the CPU clock.
        * @details Backs the profile window's calibrated/estimated badge. Reports Estimated on a
        * backend with no calibration API, and also after a sampled pair was found to disagree with
        * the submissions it was meant to explain.
        */
        VkmGpuClockCorrelation getClockCorrelation() const;

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
        * @brief Deep copy of the collected frame carrying `frameNumber`.
        * @details How the profile window pairs a CPU frame with its GPU counterpart: both
        * profilers number the same frame-loop iteration alike. Returns false for a frame the GPU
        * has not finished yet, which is the normal case for the newest few.
        * @param frameNumber Frame number to look for.
        * @param outFrame Receives the frame.
        * @return False when no collected frame carries that number.
        */
        bool copyFrameByNumber(uint32_t frameNumber, VkmGpuProfileFrame& outFrame) const;

        /*
        * @brief Drops every collected frame.
        * @details Frame numbering is not restarted, the same as VkmCpuProfiler::clear():
        * submissions recorded before the clear are still in flight and already carry their
        * numbers, and reusing those numbers would merge them into unrelated frames.
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

        // One submit() waiting for the GPU to finish it, so its marker can be told where the work
        // actually started. Kept apart from _pending because most submits never open a submission
        // at all -- uploads and acceleration structure builds record no zones.
        struct PendingMarker
        {
            VkmGpuEventTimelineObject _timeline;
            uint32_t _frameNumber = 0;
            VkmCommandQueueType _queueType = VkmCommandQueueType::Graphics;
            uint32_t _queueIndex = 0;
            std::string _queueName;
            VkmGpuSubmitMarker _marker;
        };

        // Stack entry for a zone that was opened past kMaxZonesPerSubmission and therefore has no
        // slots. It is still pushed so that open/close stay paired.
        static constexpr uint32_t kSuppressedZone = INVALID_VALUE32;

        // Markers waiting on the GPU. Bounded so a caller that never calls collect() cannot grow
        // it without limit; the cap is generous against the handful of submits a frame makes.
        static constexpr size_t kMaxPendingMarkers = 64;

        // A submission cannot start before the CPU handed it over, and cannot sit on a queue for a
        // second. Either says the sampled clock pair does not describe the counter the zones came
        // from, which is the one thing the calibration cannot verify up front.
        static constexpr uint64_t kClockSanitySlackNs = 1'000'000;
        static constexpr uint64_t kClockSanityHorizonNs = 1'000'000'000;

        // Moves one finished submission's resolved zones into the ring (or drops them when not
        // capturing), and always latches getLastFrameGpuTimeMs().
        void retireSubmission(PendingSubmission& submission, std::vector<VkmGpuProfileZone>&& zones);
        // Ring entry for `frameNumber`, appending a new one when the back entry is a different
        // frame. Frame numbers arrive monotonically, so no lookup by key is needed.
        VkmGpuProfileFrame& frameForNumber(uint32_t frameNumber);
        // Queue row within a frame, appended when the frame has not seen that queue yet. Two
        // windows submitting to one queue in a frame share a row.
        VkmGpuQueueTimeline& queueForSubmission(VkmGpuProfileFrame& frame, VkmCommandQueueType queueType,
                                                uint32_t queueIndex, const std::string& queueName);
        // Re-anchors the CPU/GPU clock pair. No-op on a backend with no calibration API.
        void sampleClockCalibration();
        // Pops the pending marker belonging to `timeline`, or null when the submission was made
        // before capture began. Markers retire in submit order, so it is at or near the front.
        PendingMarker* findPendingMarker(const VkmGpuEventTimelineObject& timeline);
        // Moves every marker the GPU has finished into the ring. Runs after the submission loop,
        // which is what has already filled in where the work started.
        void drainPendingMarkers();

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

        VkmGpuClockCalibration _calibration;
        // Cleared for the rest of the process once a mapped submission lands somewhere the CPU
        // says it cannot have; every frame from then on is anchored to its submit instead.
        bool _calibrationTrusted = true;
        bool _calibrationMismatchLogged = false;
        std::deque<PendingMarker> _pendingMarkers;
        bool _markerExhaustionLogged = false;
    };
} // namespace vkm
