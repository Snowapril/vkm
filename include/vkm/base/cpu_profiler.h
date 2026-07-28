// Copyright (c) 2025 Snowapril

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    /*
    * @brief One closed instrumented scope on one thread.
    * @details Timestamps are wall-clock nanoseconds from a steady clock, relative to the
    * profiler's process-wide epoch. Wall clock rather than per-thread CPU time on purpose:
    * a flame chart's whole point is to show where a frame went, and time spent blocked on a
    * fence or a mutex is exactly what needs to be visible.
    */
    struct VkmProfileZone
    {
        // Not owned. Either a string literal from VKM_PROFILE_SCOPE or a pointer interned by
        // VkmCpuProfiler::internName(), both of which outlive the frame ring.
        const char* _name = nullptr;
        uint64_t _beginNs = 0;
        uint64_t _endNs = 0;
        uint16_t _depth = 0; // 0 = outermost scope open on that thread
    };

    // One thread's zones within a single collected frame, sorted by begin time (parent first).
    struct VkmProfileThreadTimeline
    {
        uint64_t _threadId = 0;
        std::string _threadName;
        std::vector<VkmProfileZone> _zones;
        // True when the thread hit kMaxZonesPerThreadPerFrame and stopped recording; the UI
        // surfaces it rather than silently showing a truncated timeline.
        bool _overflowed = false;
    };

    /*
    * @brief Everything collected between two VkmCpuProfiler::beginFrame() calls.
    * @details _beginNs/_endNs span the union of every zone in the frame, not just the frame
    * driver's own bracket: threads that do not align to frame boundaries (the deferred
    * resource reclaimer polls every 4 ms) would otherwise be clipped out of the chart.
    */
    struct VkmProfileFrame
    {
        uint32_t _frameNumber = 0;
        uint64_t _beginNs = 0;
        uint64_t _endNs = 0;
        std::vector<VkmProfileThreadTimeline> _threads;

        inline uint64_t getDurationNs() const { return (_endNs > _beginNs) ? (_endNs - _beginNs) : 0; }
    };

    // Cheap per-frame row for the history strip, so the UI does not deep-copy the whole ring
    // every time it draws.
    struct VkmProfileFrameSummary
    {
        uint32_t _frameNumber = 0;
        uint64_t _durationNs = 0;
    };

    // How long one scope name was running inside a queried time range, and how many zones
    // contributed. `_name` is borrowed from the zones it came from, so it lives as long as they do.
    struct VkmProfileScopeTotal
    {
        const char* _name = nullptr;
        uint64_t _totalNs = 0;
        uint32_t _count = 0;
    };

    /*
    * @brief Totals how long each scope was running inside [beginNs, endNs], longest first.
    *
    * Zones are clipped to the range rather than counted whole, so a scope straddling either
    * edge contributes only the part inside -- which is what makes the numbers describe the
    * range the caller asked about instead of the zones that happen to touch it.
    *
    * Nesting is counted at every level: a parent and its children each contribute their own
    * overlap, so the totals deliberately sum to more than the range's duration (and more again
    * per thread). Grouping is by name text, not by the interned pointer, so one scope compiled
    * into two translation units still lands on a single row.
    */
    std::vector<VkmProfileScopeTotal> vkmAggregateProfileRange(const VkmProfileFrame& frame,
                                                              uint64_t beginNs, uint64_t endNs);

    /*
    * @brief Process-wide collector of nested per-thread CPU scopes, recorded only while
    * capturing, and kept as a ring of the most recent frames for live inspection.
    *
    * Instrument code with VKM_PROFILE_SCOPE (see the macros at the bottom of this header);
    * drive frame boundaries with beginFrame() from whichever thread runs the frame loop
    * (VkmEngine::loopInner). The ImGui front end is VkmCpuProfilerInspector, deliberately a
    * separate type so this half stays ImGui-free and unit-testable -- the same split as
    * memory_report.h vs imgui/memory_inspector.h.
    *
    * While not capturing, a scope costs one relaxed atomic load and nothing else.
    */
    class VkmCpuProfiler
    {
    public:
        // Immortal singleton (placement-new into static storage, never destroyed) -- the same
        // idiom as LoggerManager/MemoryTracker/GlobalVariableManager, so a scope in a static
        // initializer or on a thread still running at exit can never touch a destroyed object.
        static VkmCpuProfiler& singleton();

        VkmCpuProfiler(const VkmCpuProfiler&) = delete;
        VkmCpuProfiler& operator=(const VkmCpuProfiler&) = delete;

        // Turning capture on drops previously collected frames, so a capture never starts with
        // the tail of the last one. Scopes already open when this is called are not recorded
        // (each scope decides at construction), which keeps push/pop balanced across the flip.
        void setCapturing(bool capturing);
        inline bool isCapturing() const { return _capturing.load(std::memory_order_relaxed); }

        /*
        * @brief Closes the frame that was being collected and starts a new one. Call once per
        * frame from the thread driving the frame loop. No-op while not capturing.
        * @details Drains every registered thread's closed-zone buffer into a new
        * VkmProfileFrame appended to the ring (oldest dropped past kMaxFrameHistory). Zones
        * still open at this moment stay on their thread's stack and land in the next frame.
        */
        void beginFrame();

        // Number of collected frames currently in the ring.
        size_t getFrameCount() const;

        // Frame number + duration for every frame in the ring, oldest first.
        std::vector<VkmProfileFrameSummary> copyFrameSummaries() const;

        // Deep copy of one collected frame by ring index (0 = oldest). Returns false when
        // `index` is out of range.
        bool copyFrame(size_t index, VkmProfileFrame& outFrame) const;

        // Drops every collected frame and restarts frame numbering at 0. Scopes open on other
        // threads are left alone and will close into the next frame.
        void clear();

        /*
        * @brief Names the calling thread for the profile UI. Threads that never call this show
        * up as "Thread <id>". Safe to call when not capturing (names are not capture state).
        */
        static void setThreadName(const char* name);

        /*
        * @brief Returns a pointer to a copy of `name` that lives for the rest of the process.
        * @details VkmProfileZone stores names by pointer, but the frame ring outlives most
        * runtime-built strings (a render subgraph's name is rebuilt every frame). Interning is
        * what makes VKM_PROFILE_SCOPE_DYNAMIC safe. The name set is expected to be small and
        * bounded; do not intern per-call unique strings.
        */
        static const char* internName(const std::string& name);

        // VkmProfileScope's plumbing; prefer the macros. popZone() tolerates an unmatched call
        // (it no-ops), which is what lets capture be toggled off with scopes still open.
        static void pushZone(const char* name);
        static void popZone();

        // ~4 seconds of history at 60 fps.
        static constexpr size_t kMaxFrameHistory = 240;
        // Scopes nested deeper than this still open and close, they just produce no entry.
        static constexpr uint16_t kMaxZoneDepth = 32;
        // Per-thread, per-frame recording cap; hitting it sets the timeline's overflow flag.
        static constexpr size_t kMaxZonesPerThreadPerFrame = 4096;

    private:
        VkmCpuProfiler() = default;
        ~VkmCpuProfiler() = default;

        // The only member: isCapturing() is on the hot path of every instrumented scope, so it
        // must inline to a single relaxed load. Everything else this class owns (the thread
        // registry, the frame ring, the name pool) lives in cpu_profiler.cpp, which keeps the
        // mutex/deque/unordered_set machinery out of a header included engine-wide.
        std::atomic<bool> _capturing{false};
    };

    /*
    * @brief RAII scope timer. Prefer the VKM_PROFILE_SCOPE macros over constructing this
    * directly. `name` must outlive the frame ring -- a string literal, or a pointer from
    * VkmCpuProfiler::internName().
    */
    class VkmProfileScope
    {
    public:
        explicit VkmProfileScope(const char* name)
            : _active(VkmCpuProfiler::singleton().isCapturing())
        {
            if (_active)
            {
                VkmCpuProfiler::pushZone(name);
            }
        }

        ~VkmProfileScope()
        {
            if (_active)
            {
                VkmCpuProfiler::popZone();
            }
        }

        VkmProfileScope(const VkmProfileScope&) = delete;
        VkmProfileScope& operator=(const VkmProfileScope&) = delete;

    private:
        // Snapshotted at construction so a capture toggled off mid-scope cannot leave a push
        // without its pop, nor a pop without its push.
        bool _active;
    };
} // namespace vkm

#define VKM_PROFILE_CONCAT_INNER(a, b) a##b
#define VKM_PROFILE_CONCAT(a, b) VKM_PROFILE_CONCAT_INNER(a, b)

/*
* @brief Time the enclosing scope under a string-literal name.
*
*   void VkmEngine::render(double deltaTime)
*   {
*       VKM_PROFILE_SCOPE("Engine::render");
*       ...
*   }
*/
#define VKM_PROFILE_SCOPE(literalName) \
    ::vkm::VkmProfileScope VKM_PROFILE_CONCAT(_vkmProfileScope_, __LINE__)(literalName)

/*
* @brief Time the enclosing scope under a runtime-built name (std::string or const char*).
* The name is interned, so only use this where the set of distinct names is small and bounded.
*/
#define VKM_PROFILE_SCOPE_DYNAMIC(name) \
    ::vkm::VkmProfileScope VKM_PROFILE_CONCAT(_vkmProfileScope_, __LINE__)( \
        ::vkm::VkmCpuProfiler::singleton().isCapturing() ? ::vkm::VkmCpuProfiler::internName(name) : "")

// Time the enclosing function under its own name.
#define VKM_PROFILE_FUNCTION() VKM_PROFILE_SCOPE(__func__)

// Label the calling thread in the profile UI. Call once, where the thread starts.
#define VKM_PROFILE_SET_THREAD_NAME(name) ::vkm::VkmCpuProfiler::setThreadName(name)
