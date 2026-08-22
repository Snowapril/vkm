// Copyright (c) 2026 Snowapril

#include <vkm/base/cpu_profiler.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(ENABLE_CHROME_TRACING)
#include <nlohmann/json.hpp>
#endif // ENABLE_CHROME_TRACING

namespace vkm
{
    namespace
    {
        /*
        * @brief One thread's live recording state.
        * @details Ownership split, and the reason there is no lock on the hot path:
        *   - _openStack is touched only by the owning thread (push/pop), never by the collector.
        *   - _closed/_overflowed/_name are shared with the collector and guarded by _mutex.
        * A ThreadState is never destroyed (see ProfilerState::_threads), so a thread exiting
        * mid-capture cannot invalidate a buffer beginFrame() is about to drain.
        */
        struct ThreadState
        {
            uint64_t _threadId = 0;
            std::vector<VkmProfileZone> _openStack;

            mutable std::mutex _mutex;
            std::string _name;
            std::vector<VkmProfileZone> _closed;
            bool _overflowed = false;
        };

        struct ProfilerState
        {
            mutable std::mutex _registryMutex;
            std::vector<std::unique_ptr<ThreadState>> _threads;

            mutable std::mutex _frameMutex;
            std::deque<VkmProfileFrame> _frames;
            uint32_t _nextFrameNumber = 0;

            mutable std::mutex _nameMutex;
            // Node-based, so an interned string's characters never move once inserted -- that
            // stability is exactly what lets VkmProfileZone hold the c_str() by pointer.
            std::unordered_set<std::string> _names;
        };

        // Immortal, for the same reason VkmCpuProfiler itself is: threads still running at
        // static-destruction time must not touch a destroyed registry.
        ProfilerState& profilerState()
        {
            alignas(ProfilerState) static unsigned char storage[sizeof(ProfilerState)];
            static ProfilerState* instance = ::new (storage) ProfilerState();
            return *instance;
        }

        uint64_t nowNs()
        {
            // Epoch taken on first use so timestamps stay small and comfortably fit a uint64.
            static const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - epoch).count());
        }

        ThreadState* createThreadState()
        {
            ProfilerState& state = profilerState();

            auto owned = std::make_unique<ThreadState>();
            ThreadState* threadState = owned.get();
            threadState->_threadId = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            threadState->_name = "Thread " + std::to_string(threadState->_threadId);

            std::lock_guard<std::mutex> lock(state._registryMutex);
            state._threads.push_back(std::move(owned));
            return threadState;
        }

        ThreadState& threadState()
        {
            static thread_local ThreadState* cached = createThreadState();
            return *cached;
        }
    } // namespace

    VkmCpuProfiler& VkmCpuProfiler::singleton()
    {
        // Immortal singleton: placement-new into static storage, never destroyed -- same idiom
        // as LoggerManager/MemoryTracker/GlobalVariableManager.
        alignas(VkmCpuProfiler) static unsigned char storage[sizeof(VkmCpuProfiler)];
        static VkmCpuProfiler* instance = ::new (storage) VkmCpuProfiler();
        return *instance;
    }

    void VkmCpuProfiler::setCapturing(const bool capturing)
    {
        if (capturing == isCapturing())
        {
            return;
        }

        if (capturing)
        {
            // Drop the previous capture's frames and any zones that closed while idle, so the
            // first frame of a new capture is not polluted by stale data.
            clear();

            ProfilerState& state = profilerState();
            std::lock_guard<std::mutex> registryLock(state._registryMutex);
            for (std::unique_ptr<ThreadState>& thread : state._threads)
            {
                std::lock_guard<std::mutex> lock(thread->_mutex);
                thread->_closed.clear();
                thread->_overflowed = false;
            }
        }

        _capturing.store(capturing, std::memory_order_relaxed);
    }

    void VkmCpuProfiler::pushZone(const char* name)
    {
        ThreadState& thread = threadState();

        VkmProfileZone zone;
        zone._name = name;
        zone._depth = static_cast<uint16_t>(std::min<size_t>(thread._openStack.size(), UINT16_MAX));
        zone._beginNs = nowNs();
        thread._openStack.push_back(zone);
    }

    void VkmCpuProfiler::popZone()
    {
        ThreadState& thread = threadState();
        if (thread._openStack.empty())
        {
            // Unmatched pop: capture was turned on between this scope's construction and its
            // destruction, or clear() ran in between. Nothing to record.
            return;
        }

        VkmProfileZone zone = thread._openStack.back();
        thread._openStack.pop_back();
        zone._endNs = nowNs();

        if (zone._depth >= kMaxZoneDepth)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(thread._mutex);
        if (thread._closed.size() >= kMaxZonesPerThreadPerFrame)
        {
            thread._overflowed = true;
            return;
        }
        thread._closed.push_back(zone);
    }

    void VkmCpuProfiler::setThreadName(const char* name)
    {
        ThreadState& thread = threadState();
        std::lock_guard<std::mutex> lock(thread._mutex);
        thread._name = name;
    }

    const char* VkmCpuProfiler::internName(const std::string& name)
    {
        ProfilerState& state = profilerState();
        std::lock_guard<std::mutex> lock(state._nameMutex);
        return state._names.insert(name).first->c_str();
    }

    void VkmCpuProfiler::beginFrame()
    {
        if (!isCapturing())
        {
            return;
        }

        ProfilerState& state = profilerState();

        VkmProfileFrame frame;
        uint64_t minBeginNs = UINT64_MAX;
        uint64_t maxEndNs = 0;

        {
            std::lock_guard<std::mutex> registryLock(state._registryMutex);
            for (std::unique_ptr<ThreadState>& thread : state._threads)
            {
                VkmProfileThreadTimeline timeline;
                timeline._threadId = thread->_threadId;
                {
                    std::lock_guard<std::mutex> lock(thread->_mutex);
                    timeline._threadName = thread->_name;
                    timeline._zones.swap(thread->_closed);
                    timeline._overflowed = thread->_overflowed;
                    thread->_overflowed = false;
                }

                if (timeline._zones.empty())
                {
                    continue;
                }

                // Zones are appended as they close, i.e. innermost first. Sort into begin order
                // (depth breaking ties so a parent always precedes a child that started in the
                // same nanosecond) -- that is the order the flame chart walks.
                std::sort(timeline._zones.begin(), timeline._zones.end(),
                          [](const VkmProfileZone& lhs, const VkmProfileZone& rhs) {
                              return (lhs._beginNs != rhs._beginNs) ? (lhs._beginNs < rhs._beginNs)
                                                                    : (lhs._depth < rhs._depth);
                          });

                minBeginNs = std::min(minBeginNs, timeline._zones.front()._beginNs);
                for (const VkmProfileZone& zone : timeline._zones)
                {
                    maxEndNs = std::max(maxEndNs, zone._endNs);
                }

                frame._threads.push_back(std::move(timeline));
            }
        }

        if (frame._threads.empty())
        {
            // Nothing ran through an instrumented scope this frame; still record the frame so
            // the history strip stays one bar per frame.
            minBeginNs = maxEndNs = nowNs();
        }
        frame._beginNs = minBeginNs;
        frame._endNs = std::max(maxEndNs, minBeginNs);

        std::lock_guard<std::mutex> frameLock(state._frameMutex);
        frame._frameNumber = state._nextFrameNumber++;
        state._frames.push_back(std::move(frame));
        while (state._frames.size() > kMaxFrameHistory)
        {
            state._frames.pop_front();
        }
    }

    size_t VkmCpuProfiler::getFrameCount() const
    {
        ProfilerState& state = profilerState();
        std::lock_guard<std::mutex> lock(state._frameMutex);
        return state._frames.size();
    }

    std::vector<VkmProfileFrameSummary> VkmCpuProfiler::copyFrameSummaries() const
    {
        ProfilerState& state = profilerState();
        std::lock_guard<std::mutex> lock(state._frameMutex);

        std::vector<VkmProfileFrameSummary> summaries;
        summaries.reserve(state._frames.size());
        for (const VkmProfileFrame& frame : state._frames)
        {
            summaries.push_back({ frame._frameNumber, frame.getDurationNs() });
        }
        return summaries;
    }

    bool VkmCpuProfiler::copyFrame(const size_t index, VkmProfileFrame& outFrame) const
    {
        ProfilerState& state = profilerState();
        std::lock_guard<std::mutex> lock(state._frameMutex);
        if (index >= state._frames.size())
        {
            return false;
        }
        outFrame = state._frames[index];
        return true;
    }

    void VkmCpuProfiler::clear()
    {
        ProfilerState& state = profilerState();
        std::lock_guard<std::mutex> lock(state._frameMutex);
        state._frames.clear();
        // Restart numbering so each capture's first frame reads as #0 in the UI rather than
        // continuing a count from a capture the user already discarded.
        state._nextFrameNumber = 0;
    }

    std::vector<VkmProfileScopeTotal> vkmAggregateProfileRange(const VkmProfileFrame& frame,
                                                              const uint64_t beginNs, const uint64_t endNs)
    {
        if (endNs <= beginNs)
        {
            return {};
        }

        std::unordered_map<std::string_view, VkmProfileScopeTotal> totals;
        for (const VkmProfileThreadTimeline& timeline : frame._threads)
        {
            for (const VkmProfileZone& zone : timeline._zones)
            {
                if (zone._name == nullptr)
                {
                    continue;
                }
                const uint64_t overlapBegin = std::max(zone._beginNs, beginNs);
                const uint64_t overlapEnd = std::min(zone._endNs, endNs);
                if (overlapEnd <= overlapBegin)
                {
                    continue;
                }

                VkmProfileScopeTotal& entry = totals[std::string_view(zone._name)];
                // Whichever pointer arrived first wins; they all spell the same name, and the
                // zones that own them outlive this result.
                if (entry._name == nullptr)
                {
                    entry._name = zone._name;
                }
                entry._totalNs += overlapEnd - overlapBegin;
                entry._count += 1;
            }
        }

        std::vector<VkmProfileScopeTotal> sorted;
        sorted.reserve(totals.size());
        for (const auto& [name, entry] : totals)
        {
            (void)name;
            sorted.push_back(entry);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const VkmProfileScopeTotal& lhs, const VkmProfileScopeTotal& rhs) {
                      return lhs._totalNs > rhs._totalNs;
                  });
        return sorted;
    }

    bool VkmCpuProfiler::exportChromeTrace(const std::string& path) const
    {
#if defined(ENABLE_CHROME_TRACING)
        // Chrome's pid/tid are display groupings, not OS identifiers, so one process row is
        // enough for an in-process profiler.
        constexpr int kProcessId = 1;

        // _threadId is a 64-bit hash of std::thread::id, which the viewer's JavaScript cannot
        // represent exactly. Hand out small sequential ids in first-seen order instead, so a
        // thread keeps one row across every exported frame.
        std::unordered_map<uint64_t, int> threadRowIds;
        nlohmann::json events = nlohmann::json::array();

        // Built through the public frame accessors, each of which takes and releases the frame
        // mutex on its own -- beginFrame() must never be left waiting on this export's file I/O.
        const size_t frameCount = getFrameCount();
        for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            VkmProfileFrame frame;
            if (!copyFrame(frameIndex, frame))
            {
                continue;
            }

            for (const VkmProfileThreadTimeline& timeline : frame._threads)
            {
                auto [row, inserted] =
                    threadRowIds.emplace(timeline._threadId, static_cast<int>(threadRowIds.size()));
                if (inserted)
                {
                    events.push_back({{"ph", "M"},
                                      {"name", "thread_name"},
                                      {"pid", kProcessId},
                                      {"tid", row->second},
                                      {"args", {{"name", timeline._threadName}}}});
                }

                for (const VkmProfileZone& zone : timeline._zones)
                {
                    // Complete events nest by containment on a row, and beginFrame() already
                    // sorts each thread's zones by (begin, depth), so no explicit nesting.
                    events.push_back({{"ph", "X"},
                                      {"cat", "cpu"},
                                      {"name", zone._name != nullptr ? zone._name : ""},
                                      {"pid", kProcessId},
                                      {"tid", row->second},
                                      {"ts", static_cast<double>(zone._beginNs) / 1000.0},
                                      {"dur", static_cast<double>(zone._endNs - zone._beginNs) / 1000.0}});
                }
            }
        }

        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out << nlohmann::json{{"traceEvents", std::move(events)}}.dump();
        return out.good();
#else
        (void)path;
        return false;
#endif // ENABLE_CHROME_TRACING
    }
} // namespace vkm
