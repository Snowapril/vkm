// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/gpu_profiler.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/driver.h>

#include <algorithm>
#include <fstream>
#include <string_view>
#include <unordered_map>

#if defined(ENABLE_CHROME_TRACING)
#include <nlohmann/json.hpp>
#endif // ENABLE_CHROME_TRACING

namespace vkm
{
    namespace
    {
        inline double nsToMs(const uint64_t ns)
        {
            return static_cast<double>(ns) * 1e-6;
        }

        // Zones are recorded in begin order but the chart walks them by (begin, depth) so that a
        // parent always precedes a child that started in the same nanosecond -- the same ordering
        // VkmCpuProfiler::beginFrame() establishes for CPU zones.
        void sortZones(std::vector<VkmGpuProfileZone>& zones)
        {
            std::sort(zones.begin(), zones.end(),
                      [](const VkmGpuProfileZone& lhs, const VkmGpuProfileZone& rhs) {
                          return (lhs._beginNs != rhs._beginNs) ? (lhs._beginNs < rhs._beginNs)
                                                                : (lhs._depth < rhs._depth);
                      });
        }
    } // namespace

    VkmGpuProfiler::VkmGpuProfiler(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmGpuProfiler::~VkmGpuProfiler()
    {
    }

    bool VkmGpuProfiler::initialize()
    {
        _supported = _driver->initializeGpuTimestampPool(kTimestampSlotCount);
        if (_supported)
        {
            _timestampPeriodNs = _driver->getGpuTimestampPeriodNs();
        }
        else
        {
            VKM_DEBUG_INFO("GPU timestamps are not available on this backend; GPU profiling is disabled");
        }
        return true;
    }

    void VkmGpuProfiler::destroy()
    {
        if (_supported)
        {
            _driver->destroyGpuTimestampPool();
            _supported = false;
        }
        _pending = {};
        _oldestBucket = 0;
        _pendingCount = 0;
        _frames.clear();
    }

    uint32_t VkmGpuProfiler::beginSubmission(VkmCommandQueueBase* queue, VkmCommandBufferBase* commandBuffer,
                                             const uint32_t zoneCount)
    {
        if (!_supported || queue == nullptr || commandBuffer == nullptr || zoneCount == 0)
        {
            return kInvalidSubmission;
        }

        if (_pendingCount >= kMaxPendingSubmissions)
        {
            // Every slot bucket still belongs to a submission that has not been retired. Timing
            // this one would overwrite timestamps that are about to be read.
            if (!_slotExhaustionLogged)
            {
                // Once, not per frame: a caller driving VkmRenderGraph without ever calling
                // collect() would otherwise warn on every submission forever.
                _slotExhaustionLogged = true;
                VKM_DEBUG_WARN("GPU profiler has no free timestamp slots; submissions will go "
                               "untimed until collect() retires some (logged once)");
            }
            return kInvalidSubmission;
        }

        const uint32_t bucket = (_oldestBucket + _pendingCount) % kMaxPendingSubmissions;
        ++_pendingCount;

        PendingSubmission& submission = _pending[bucket];
        submission = PendingSubmission{};
        submission._firstSlot = bucket * 2 * kMaxZonesPerSubmission;
        submission._frameNumber = _recordFrameNumber;
        submission._queueType = queue->getQueueType();
        submission._queueIndex = queue->getQueueIndex();
        submission._queueName = (queue->getQueueName() != nullptr) ? queue->getQueueName() : "";
        submission._overflowed = zoneCount > kMaxZonesPerSubmission;
        submission._zones.reserve(std::min(zoneCount, kMaxZonesPerSubmission));

        // Reset exactly the slots this submission will write, and no more: a slot that is reset
        // but never written stays permanently unavailable to Vulkan's query readback.
        const uint32_t slotCount = 2 * std::min(zoneCount, kMaxZonesPerSubmission);
        _driver->resetGpuTimestampSlots(commandBuffer, submission._firstSlot, slotCount);

        return bucket;
    }

    void VkmGpuProfiler::beginZone(VkmCommandBufferBase* commandBuffer, const uint32_t submissionIndex,
                                   const char* name, const uint32_t subGraphId, const uint16_t depth)
    {
        if (!_supported || submissionIndex >= kMaxPendingSubmissions)
        {
            return;
        }

        PendingSubmission& submission = _pending[submissionIndex];
        if (submission._zones.size() >= kMaxZonesPerSubmission)
        {
            submission._overflowed = true;
            // Still pushed, so the matching endZone() closes *this* zone rather than the one
            // enclosing it -- an unbalanced stack would pair a subgraph's end timestamp with the
            // submission zone's begin and report a span neither of them ran for.
            submission._openZoneStack.push_back(kSuppressedZone);
            return;
        }

        const uint32_t zoneIndex = static_cast<uint32_t>(submission._zones.size());
        submission._zones.push_back(PendingZone{ name, subGraphId, depth });
        submission._openZoneStack.push_back(zoneIndex);

        // Both slots go in up front because WebGPU can only express a timestamp as a pass
        // descriptor's beginning/end pair, which must be filled before the pass is opened.
        commandBuffer->beginGpuZone(submission._firstSlot + zoneIndex * 2,
                                    submission._firstSlot + zoneIndex * 2 + 1);
    }

    void VkmGpuProfiler::endZone(VkmCommandBufferBase* commandBuffer, const uint32_t submissionIndex)
    {
        if (!_supported || submissionIndex >= kMaxPendingSubmissions)
        {
            return;
        }

        PendingSubmission& submission = _pending[submissionIndex];
        if (submission._openZoneStack.empty())
        {
            // Unmatched close -- nothing opened this zone.
            return;
        }

        const uint32_t zoneIndex = submission._openZoneStack.back();
        submission._openZoneStack.pop_back();
        if (zoneIndex != kSuppressedZone)
        {
            // Recorded per zone rather than per submission: on WebGPU a subgraph that opened a
            // pass is timed while the submission-wide zone around it is not.
            submission._zones[zoneIndex]._timed = commandBuffer->endGpuZone();
        }

        if (submission._openZoneStack.empty())
        {
            // The outermost zone just closed, so nothing else will be recorded into this
            // submission. Backends that need a resolve command in the same command buffer
            // (WebGPU) get their chance here, before endCommandBuffer().
            const uint32_t slotCount = 2 * static_cast<uint32_t>(submission._zones.size());
            commandBuffer->resolveGpuZones(submission._firstSlot, slotCount);
        }
    }

    void VkmGpuProfiler::endSubmission(const uint32_t submissionIndex, const VkmGpuEventTimelineObject& timeline)
    {
        if (!_supported || submissionIndex >= kMaxPendingSubmissions)
        {
            return;
        }

        PendingSubmission& submission = _pending[submissionIndex];
        submission._timeline = timeline;
        submission._submitted = true;
    }

    void VkmGpuProfiler::collect()
    {
        while (_supported && _pendingCount > 0)
        {
            PendingSubmission& submission = _pending[_oldestBucket];
            if (!submission._submitted)
            {
                // Still being recorded. Buckets retire in submission order, so nothing behind
                // this one can be ready either.
                break;
            }

            VkmGpuEventTimelineBase* timeline = submission._timeline._gpuEventTimeline;
            // queryLastCompletedTimeline() is the same non-blocking "has the GPU passed this
            // point" question the resource pool's usage tracking asks.
            if (timeline != nullptr &&
                timeline->queryLastCompletedTimeline() < submission._timeline._timelineValue)
            {
                break;
            }

            std::vector<VkmGpuProfileZone> zones;
            const uint32_t zoneCount = static_cast<uint32_t>(submission._zones.size());
            if (zoneCount > 0)
            {
                std::array<uint64_t, 2 * kMaxZonesPerSubmission> ticks{};
                if (_driver->resolveGpuTimestamps(submission._firstSlot, zoneCount * 2, ticks.data()))
                {
                    zones.reserve(zoneCount);
                    for (uint32_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
                    {
                        const PendingZone& pending = submission._zones[zoneIndex];
                        if (pending._timed == false)
                        {
                            // The backend never wrote this pair, so its slots hold nothing this
                            // submission put there. Showing it would be inventing a measurement.
                            continue;
                        }

                        const uint64_t beginTicks = ticks[zoneIndex * 2];
                        const uint64_t endTicks = ticks[zoneIndex * 2 + 1];

                        VkmGpuProfileZone zone;
                        zone._name = pending._name;
                        zone._subGraphId = pending._subGraphId;
                        zone._depth = pending._depth;
                        zone._beginNs = static_cast<uint64_t>(static_cast<double>(beginTicks) * _timestampPeriodNs);
                        zone._endNs = static_cast<uint64_t>(static_cast<double>(endTicks) * _timestampPeriodNs);
                        if (zone._endNs < zone._beginNs)
                        {
                            // Nothing legitimate produces this; drop it rather than show a zone
                            // that ended before it started.
                            continue;
                        }
                        zones.push_back(zone);
                    }
                }
            }

            retireSubmission(submission, std::move(zones));

            submission = PendingSubmission{};
            _oldestBucket = (_oldestBucket + 1) % kMaxPendingSubmissions;
            --_pendingCount;
        }

        // Stamped onto submissions recorded from here on, so one ring entry corresponds to one
        // frame-loop iteration the same way a VkmProfileFrame does.
        ++_recordFrameNumber;
    }

    void VkmGpuProfiler::retireSubmission(PendingSubmission& submission, std::vector<VkmGpuProfileZone>&& zones)
    {
        if (!zones.empty())
        {
            // The submission's whole span rather than its depth-0 zone: WebGPU cannot time a
            // zone that encloses no pass, so there is not always a depth-0 zone to read.
            // Latched regardless of _capturing -- this is what the always-visible debug overlay
            // shows, and the inspector window may well be closed.
            uint64_t spanBeginNs = UINT64_MAX;
            uint64_t spanEndNs = 0;
            for (const VkmGpuProfileZone& zone : zones)
            {
                spanBeginNs = std::min(spanBeginNs, zone._beginNs);
                spanEndNs = std::max(spanEndNs, zone._endNs);
            }
            _lastFrameGpuTimeMs = nsToMs((spanEndNs > spanBeginNs) ? (spanEndNs - spanBeginNs) : 0);
        }

        // Latched on the same terms as _lastFrameGpuTimeMs: the render graph inspector reads the
        // averages whether or not this profiler's own window is capturing.
        _subGraphAverages.addZones(zones);

        if (!_capturing || (zones.empty() && !submission._overflowed))
        {
            return;
        }

        VkmGpuProfileFrame& frame = frameForNumber(submission._frameNumber);

        // Two windows submitting to the same queue in one frame produce two submissions; they
        // belong on one row, not two.
        auto queueIt = std::find_if(frame._queues.begin(), frame._queues.end(),
                                    [&submission](const VkmGpuQueueTimeline& timeline) {
                                        return timeline._queueType == submission._queueType &&
                                               timeline._queueIndex == submission._queueIndex;
                                    });
        if (queueIt == frame._queues.end())
        {
            VkmGpuQueueTimeline timeline;
            timeline._queueType = submission._queueType;
            timeline._queueIndex = submission._queueIndex;
            timeline._queueName = submission._queueName;
            frame._queues.push_back(std::move(timeline));
            queueIt = std::prev(frame._queues.end());
        }

        queueIt->_overflowed = queueIt->_overflowed || submission._overflowed;
        queueIt->_zones.insert(queueIt->_zones.end(), zones.begin(), zones.end());
        sortZones(queueIt->_zones);

        // Recomputed rather than accumulated so the span always describes exactly what the frame
        // currently holds, however many submissions have landed in it so far.
        uint64_t minBeginNs = UINT64_MAX;
        uint64_t maxEndNs = 0;
        for (const VkmGpuQueueTimeline& timeline : frame._queues)
        {
            for (const VkmGpuProfileZone& zone : timeline._zones)
            {
                minBeginNs = std::min(minBeginNs, zone._beginNs);
                maxEndNs = std::max(maxEndNs, zone._endNs);
            }
        }
        if (minBeginNs == UINT64_MAX)
        {
            minBeginNs = maxEndNs = 0;
        }
        frame._beginNs = minBeginNs;
        frame._endNs = std::max(maxEndNs, minBeginNs);
    }

    VkmGpuProfileFrame& VkmGpuProfiler::frameForNumber(const uint32_t frameNumber)
    {
        if (!_frames.empty() && _frames.back()._frameNumber == frameNumber)
        {
            return _frames.back();
        }

        VkmGpuProfileFrame frame;
        frame._frameNumber = frameNumber;
        _frames.push_back(std::move(frame));
        while (_frames.size() > kMaxFrameHistory)
        {
            _frames.pop_front();
        }
        return _frames.back();
    }

    void VkmGpuProfiler::setCapturing(const bool capturing)
    {
        if (capturing == _capturing)
        {
            return;
        }

        if (capturing)
        {
            // Drop the previous capture's frames so the first frame of a new capture is not
            // polluted by stale data.
            clear();
        }
        _capturing = capturing;
    }

    size_t VkmGpuProfiler::getFrameCount() const
    {
        return _frames.size();
    }

    std::vector<VkmGpuProfileFrameSummary> VkmGpuProfiler::copyFrameSummaries() const
    {
        std::vector<VkmGpuProfileFrameSummary> summaries;
        summaries.reserve(_frames.size());
        for (const VkmGpuProfileFrame& frame : _frames)
        {
            summaries.push_back({ frame._frameNumber, frame.getDurationNs() });
        }
        return summaries;
    }

    bool VkmGpuProfiler::copyFrame(const size_t index, VkmGpuProfileFrame& outFrame) const
    {
        if (index >= _frames.size())
        {
            return false;
        }
        outFrame = _frames[index];
        return true;
    }

    void VkmGpuProfiler::clear()
    {
        _frames.clear();
    }

    void VkmGpuSubGraphAverages::addZones(const std::vector<VkmGpuProfileZone>& zones)
    {
        for (const VkmGpuProfileZone& zone : zones)
        {
            if (zone._depth != 1 || zone._subGraphId == INVALID_VALUE32 || zone._name == nullptr)
            {
                continue;
            }

            Window& window = _windows[zone._name];
            // The slot about to be overwritten holds the sample leaving the window, and holds 0
            // until the window has wrapped once.
            window._sumNs -= window._samples[window._next];
            window._samples[window._next] = zone.getDurationNs();
            window._sumNs += window._samples[window._next];
            window._next = (window._next + 1) % kWindowSize;
            window._count = std::min<uint32_t>(window._count + 1, static_cast<uint32_t>(kWindowSize));
        }
    }

    double VkmGpuSubGraphAverages::getAverageMs(const std::string& subGraphName) const
    {
        const auto it = _windows.find(subGraphName);
        if (it == _windows.end() || it->second._count == 0)
        {
            return 0.0;
        }
        return nsToMs(it->second._sumNs) / static_cast<double>(it->second._count);
    }

    uint32_t VkmGpuSubGraphAverages::getSampleCount(const std::string& subGraphName) const
    {
        const auto it = _windows.find(subGraphName);
        return (it == _windows.end()) ? 0 : it->second._count;
    }

    void VkmGpuSubGraphAverages::clear()
    {
        _windows.clear();
    }

    std::vector<VkmGpuProfileZoneTotal> vkmAggregateGpuProfileRange(const VkmGpuProfileFrame& frame,
                                                                    const uint64_t beginNs, const uint64_t endNs)
    {
        if (endNs <= beginNs)
        {
            return {};
        }

        std::unordered_map<std::string_view, VkmGpuProfileZoneTotal> totals;
        for (const VkmGpuQueueTimeline& timeline : frame._queues)
        {
            for (const VkmGpuProfileZone& zone : timeline._zones)
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

                VkmGpuProfileZoneTotal& entry = totals[std::string_view(zone._name)];
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

        std::vector<VkmGpuProfileZoneTotal> sorted;
        sorted.reserve(totals.size());
        for (const auto& [name, entry] : totals)
        {
            (void)name;
            sorted.push_back(entry);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const VkmGpuProfileZoneTotal& lhs, const VkmGpuProfileZoneTotal& rhs) {
                      return lhs._totalNs > rhs._totalNs;
                  });
        return sorted;
    }

    bool VkmGpuProfiler::exportChromeTrace(const std::string& path) const
    {
        return vkmWriteGpuChromeTrace(std::vector<VkmGpuProfileFrame>(_frames.begin(), _frames.end()), path);
    }

    bool vkmWriteGpuChromeTrace(const std::vector<VkmGpuProfileFrame>& frames, const std::string& path)
    {
#if defined(ENABLE_CHROME_TRACING)
        // The CPU export uses pid 1 (see VkmCpuProfiler::exportChromeTrace), so a different pid
        // here lets both files be loaded into one viewer without their rows colliding.
        constexpr int kProcessId = 2;

        // One row per (queue type, queue index), handed out in first-seen order so a queue keeps
        // one row across every exported frame.
        std::unordered_map<uint64_t, int> queueRowIds;
        nlohmann::json events = nlohmann::json::array();

        for (const VkmGpuProfileFrame& frame : frames)
        {
            for (const VkmGpuQueueTimeline& timeline : frame._queues)
            {
                const uint64_t queueKey =
                    (static_cast<uint64_t>(timeline._queueType) << 32) | timeline._queueIndex;
                auto [row, inserted] = queueRowIds.emplace(queueKey, static_cast<int>(queueRowIds.size()));
                if (inserted)
                {
                    events.push_back({{"ph", "M"},
                                      {"name", "thread_name"},
                                      {"pid", kProcessId},
                                      {"tid", row->second},
                                      {"args", {{"name", timeline._queueName}}}});
                }

                for (const VkmGpuProfileZone& zone : timeline._zones)
                {
                    // Complete events nest by containment on a row, and the zones are already
                    // sorted by (begin, depth), so no explicit nesting is needed.
                    events.push_back({{"ph", "X"},
                                      {"cat", "gpu"},
                                      {"name", zone._name != nullptr ? zone._name : ""},
                                      {"pid", kProcessId},
                                      {"tid", row->second},
                                      {"ts", static_cast<double>(zone._beginNs) / 1000.0},
                                      {"dur", static_cast<double>(zone.getDurationNs()) / 1000.0}});
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
        (void)frames;
        (void)path;
        return false;
#endif // ENABLE_CHROME_TRACING
    }
} // namespace vkm
