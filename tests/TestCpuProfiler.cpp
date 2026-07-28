// Copyright (c) 2025 Snowapril

#include <doctest/doctest.h>

#include <vkm/base/cpu_profiler.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(ENABLE_CHROME_TRACING)
#include <nlohmann/json.hpp>
namespace fs = std::filesystem;
#endif // ENABLE_CHROME_TRACING

using vkm::VkmCpuProfiler;
using vkm::VkmProfileFrame;
using vkm::VkmProfileFrameSummary;
using vkm::VkmProfileThreadTimeline;
using vkm::VkmProfileZone;

namespace
{
    // The profiler is a process-wide singleton shared by every test case in this binary, so each
    // case starts from a known state rather than assuming one.
    void resetProfiler()
    {
        VkmCpuProfiler::singleton().setCapturing(false);
        VkmCpuProfiler::singleton().clear();
    }

    const VkmProfileThreadTimeline* findTimeline(const VkmProfileFrame& frame, const char* threadName)
    {
        for (const VkmProfileThreadTimeline& timeline : frame._threads)
        {
            if (timeline._threadName == threadName)
            {
                return &timeline;
            }
        }
        return nullptr;
    }

    const VkmProfileZone* findZone(const VkmProfileThreadTimeline& timeline, const char* zoneName)
    {
        for (const VkmProfileZone& zone : timeline._zones)
        {
            if (std::string(zone._name) == zoneName)
            {
                return &zone;
            }
        }
        return nullptr;
    }
} // namespace

TEST_CASE("scopes recorded while not capturing produce no frames")
{
    resetProfiler();

    {
        VKM_PROFILE_SCOPE("NotCaptured");
    }
    VkmCpuProfiler::singleton().beginFrame();

    CHECK(VkmCpuProfiler::singleton().isCapturing() == false);
    CHECK(VkmCpuProfiler::singleton().getFrameCount() == 0);
}

TEST_CASE("nested scopes on multiple threads land in one frame with their nesting intact")
{
    resetProfiler();
    VkmCpuProfiler::singleton().setCapturing(true);
    VKM_PROFILE_SET_THREAD_NAME("TestMain");

    {
        VKM_PROFILE_SCOPE("Outer");
        {
            VKM_PROFILE_SCOPE("Inner");
        }
        {
            VKM_PROFILE_SCOPE("InnerSibling");
        }
    }

#if !defined(VKM_PLATFORM_WASM)
    // A second thread's zones must show up as their own timeline, not merged into the caller's.
    // Skipped on WASM, which has no worker threads (see VkmDeferredResourceReclaimer::start).
    std::thread worker([] {
        VKM_PROFILE_SET_THREAD_NAME("TestWorker");
        VKM_PROFILE_SCOPE("WorkerOuter");
        VKM_PROFILE_SCOPE("WorkerInner");
    });
    worker.join();
#endif

    // beginFrame() closes the frame everything above was recorded into.
    VkmCpuProfiler::singleton().beginFrame();
    VkmCpuProfiler::singleton().setCapturing(false);

    REQUIRE(VkmCpuProfiler::singleton().getFrameCount() == 1);

    VkmProfileFrame frame;
    REQUIRE(VkmCpuProfiler::singleton().copyFrame(0, frame));

    const VkmProfileThreadTimeline* mainTimeline = findTimeline(frame, "TestMain");
    REQUIRE(mainTimeline != nullptr);
    REQUIRE(mainTimeline->_zones.size() == 3);
    CHECK(mainTimeline->_overflowed == false);

    // Sorted by begin time, so the parent comes first and the siblings follow in call order.
    CHECK(std::string(mainTimeline->_zones[0]._name) == "Outer");
    CHECK(mainTimeline->_zones[0]._depth == 0);
    CHECK(std::string(mainTimeline->_zones[1]._name) == "Inner");
    CHECK(mainTimeline->_zones[1]._depth == 1);
    CHECK(std::string(mainTimeline->_zones[2]._name) == "InnerSibling");
    CHECK(mainTimeline->_zones[2]._depth == 1);

    // A child must be fully contained in its parent.
    const VkmProfileZone& outer = mainTimeline->_zones[0];
    const VkmProfileZone& inner = mainTimeline->_zones[1];
    CHECK(outer._endNs >= outer._beginNs);
    CHECK(inner._beginNs >= outer._beginNs);
    CHECK(inner._endNs <= outer._endNs);

#if !defined(VKM_PLATFORM_WASM)
    const VkmProfileThreadTimeline* workerTimeline = findTimeline(frame, "TestWorker");
    REQUIRE(workerTimeline != nullptr);
    REQUIRE(workerTimeline->_zones.size() == 2);
    CHECK(std::string(workerTimeline->_zones[0]._name) == "WorkerOuter");
    CHECK(workerTimeline->_zones[0]._depth == 0);
    CHECK(std::string(workerTimeline->_zones[1]._name) == "WorkerInner");
    CHECK(workerTimeline->_zones[1]._depth == 1);
    CHECK(workerTimeline->_threadId != mainTimeline->_threadId);

    // The frame's span covers every thread's zones, not just the collecting thread's.
    CHECK(frame._beginNs <= workerTimeline->_zones[0]._beginNs);
    CHECK(frame._endNs >= workerTimeline->_zones[0]._endNs);
#endif
}

TEST_CASE("a dynamic scope name outlives the string it was built from")
{
    resetProfiler();
    VkmCpuProfiler::singleton().setCapturing(true);
    VKM_PROFILE_SET_THREAD_NAME("TestMain");

    {
        const std::string temporaryName = std::string("SubGraph#") + std::to_string(7);
        VKM_PROFILE_SCOPE_DYNAMIC(temporaryName);
    } // temporaryName is destroyed here; the interned copy must not be

    VkmCpuProfiler::singleton().beginFrame();
    VkmCpuProfiler::singleton().setCapturing(false);

    VkmProfileFrame frame;
    REQUIRE(VkmCpuProfiler::singleton().copyFrame(0, frame));
    const VkmProfileThreadTimeline* timeline = findTimeline(frame, "TestMain");
    REQUIRE(timeline != nullptr);
    CHECK(findZone(*timeline, "SubGraph#7") != nullptr);

    // Interning the same name twice must hand back the very same pointer, which is what keeps
    // the name pool bounded.
    CHECK(VkmCpuProfiler::internName("SubGraph#7") == VkmCpuProfiler::internName(std::string("SubGraph#7")));
}

TEST_CASE("the frame ring drops the oldest frames past its capacity")
{
    resetProfiler();
    VkmCpuProfiler::singleton().setCapturing(true);

    const size_t extraFrames = 5;
    for (size_t i = 0; i < VkmCpuProfiler::kMaxFrameHistory + extraFrames; ++i)
    {
        VkmCpuProfiler::singleton().beginFrame();
    }
    VkmCpuProfiler::singleton().setCapturing(false);

    CHECK(VkmCpuProfiler::singleton().getFrameCount() == VkmCpuProfiler::kMaxFrameHistory);

    const std::vector<VkmProfileFrameSummary> summaries = VkmCpuProfiler::singleton().copyFrameSummaries();
    REQUIRE(summaries.size() == VkmCpuProfiler::kMaxFrameHistory);
    // Numbering restarts at 0 on clear() and keeps counting up as frames are dropped, so the
    // surviving window is the last kMaxFrameHistory of them, contiguous.
    CHECK(summaries.front()._frameNumber == extraFrames);
    CHECK(summaries.back()._frameNumber == VkmCpuProfiler::kMaxFrameHistory + extraFrames - 1);
    for (size_t i = 1; i < summaries.size(); ++i)
    {
        CHECK(summaries[i]._frameNumber == summaries[i - 1]._frameNumber + 1);
    }

    // Out-of-range reads fail rather than returning a stale frame.
    VkmProfileFrame frame;
    CHECK(VkmCpuProfiler::singleton().copyFrame(VkmCpuProfiler::kMaxFrameHistory, frame) == false);
}

TEST_CASE("clear empties the ring and starting a capture discards the previous one")
{
    resetProfiler();
    VkmCpuProfiler::singleton().setCapturing(true);
    VkmCpuProfiler::singleton().beginFrame();
    VkmCpuProfiler::singleton().beginFrame();
    REQUIRE(VkmCpuProfiler::singleton().getFrameCount() == 2);

    VkmCpuProfiler::singleton().clear();
    CHECK(VkmCpuProfiler::singleton().getFrameCount() == 0);

    VkmCpuProfiler::singleton().beginFrame();
    REQUIRE(VkmCpuProfiler::singleton().getFrameCount() == 1);

    // Re-arming drops whatever the last capture left behind.
    VkmCpuProfiler::singleton().setCapturing(false);
    VkmCpuProfiler::singleton().setCapturing(true);
    CHECK(VkmCpuProfiler::singleton().getFrameCount() == 0);

    resetProfiler();
}

/*
* The range aggregation the profiler window's drag-selection reads. Built from hand-made
* frames rather than a real capture so the expected nanosecond totals are exact.
*/
namespace
{
vkm::VkmProfileFrame makeFrame(std::vector<vkm::VkmProfileThreadTimeline> threads)
{
    vkm::VkmProfileFrame frame;
    frame._frameNumber = 0;
    frame._beginNs = 0;
    frame._endNs = 1000;
    frame._threads = std::move(threads);
    return frame;
}

vkm::VkmProfileZone makeZone(const char* name, uint64_t beginNs, uint64_t endNs, uint16_t depth = 0)
{
    vkm::VkmProfileZone zone;
    zone._name = name;
    zone._beginNs = beginNs;
    zone._endNs = endNs;
    zone._depth = depth;
    return zone;
}

const vkm::VkmProfileScopeTotal* findTotal(const std::vector<vkm::VkmProfileScopeTotal>& totals,
                                           const char* name)
{
    for (const vkm::VkmProfileScopeTotal& entry : totals)
    {
        if (std::string(entry._name) == name)
        {
            return &entry;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("range aggregation clips zones to the range instead of counting them whole")
{
    vkm::VkmProfileThreadTimeline timeline;
    timeline._threadId = 1;
    timeline._zones = {
        makeZone("Straddles", 50, 250),  // only 150 of its 200 ns is inside
        makeZone("Inside", 120, 170),    // wholly inside
        makeZone("Outside", 300, 400),   // wholly outside
    };

    const std::vector<vkm::VkmProfileScopeTotal> totals =
        vkm::vkmAggregateProfileRange(makeFrame({ timeline }), 100, 250);

    REQUIRE(totals.size() == 2); // "Outside" contributes nothing and gets no row

    const vkm::VkmProfileScopeTotal* straddles = findTotal(totals, "Straddles");
    REQUIRE(straddles != nullptr);
    CHECK(straddles->_totalNs == 150);
    CHECK(straddles->_count == 1);

    const vkm::VkmProfileScopeTotal* inside = findTotal(totals, "Inside");
    REQUIRE(inside != nullptr);
    CHECK(inside->_totalNs == 50);

    CHECK(findTotal(totals, "Outside") == nullptr);

    // Longest first, which is what makes the top of the table the answer.
    CHECK(totals[0]._totalNs >= totals[1]._totalNs);
}

TEST_CASE("range aggregation counts nested scopes at every level and merges repeats")
{
    vkm::VkmProfileThreadTimeline timeline;
    timeline._threadId = 1;
    timeline._zones = {
        makeZone("Parent", 0, 100, 0),
        makeZone("Child", 10, 40, 1),
        makeZone("Child", 60, 90, 1), // same name again: one row, two calls
    };

    const std::vector<vkm::VkmProfileScopeTotal> totals =
        vkm::vkmAggregateProfileRange(makeFrame({ timeline }), 0, 100);

    const vkm::VkmProfileScopeTotal* child = findTotal(totals, "Child");
    REQUIRE(child != nullptr);
    CHECK(child->_totalNs == 60); // 30 + 30
    CHECK(child->_count == 2);

    const vkm::VkmProfileScopeTotal* parent = findTotal(totals, "Parent");
    REQUIRE(parent != nullptr);
    CHECK(parent->_totalNs == 100); // the parent is not reduced by what its children took

    // Nesting means the totals exceed the range's own duration; that is the documented shape.
    CHECK(parent->_totalNs + child->_totalNs > 100);
}

TEST_CASE("range aggregation sums across threads and rejects an empty range")
{
    vkm::VkmProfileThreadTimeline first;
    first._threadId = 1;
    first._zones = { makeZone("Shared", 0, 50) };

    vkm::VkmProfileThreadTimeline second;
    second._threadId = 2;
    second._zones = { makeZone("Shared", 0, 30) };

    const vkm::VkmProfileFrame frame = makeFrame({ first, second });

    const std::vector<vkm::VkmProfileScopeTotal> totals = vkm::vkmAggregateProfileRange(frame, 0, 100);
    REQUIRE(totals.size() == 1);
    CHECK(totals[0]._totalNs == 80);
    CHECK(totals[0]._count == 2);

    // A zero-width or inverted range has nothing to report rather than everything.
    CHECK(vkm::vkmAggregateProfileRange(frame, 50, 50).empty());
    CHECK(vkm::vkmAggregateProfileRange(frame, 80, 20).empty());
}

#if defined(ENABLE_CHROME_TRACING)
TEST_CASE("exportChromeTrace emits microsecond complete events and thread_name metadata")
{
    resetProfiler();
    VkmCpuProfiler::singleton().setCapturing(true);
    VKM_PROFILE_SET_THREAD_NAME("ChromeTraceThread");

    {
        vkm::VkmProfileScope outer("ChromeOuter");
        {
            vkm::VkmProfileScope inner("ChromeInner");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    VkmCpuProfiler::singleton().beginFrame();

    const fs::path tracePath = fs::temp_directory_path() / "vkm_test_chrome_trace.json";
    REQUIRE(VkmCpuProfiler::singleton().exportChromeTrace(tracePath.string()));

    std::ifstream in(tracePath);
    REQUIRE(in.good());
    const nlohmann::json trace = nlohmann::json::parse(in);
    in.close();

    const nlohmann::json& events = trace.at("traceEvents");
    REQUIRE(events.is_array());
    REQUIRE(events.empty() == false);

    const nlohmann::json* outer = nullptr;
    const nlohmann::json* inner = nullptr;
    size_t threadNameEvents = 0;
    const nlohmann::json* threadNameEvent = nullptr;
    for (const nlohmann::json& event : events)
    {
        if (event.at("ph") == "M" && event.at("name") == "thread_name" &&
            event.at("args").at("name") == "ChromeTraceThread")
        {
            ++threadNameEvents;
            threadNameEvent = &event;
        }
        else if (event.at("ph") == "X")
        {
            if (event.at("name") == "ChromeOuter") { outer = &event; }
            else if (event.at("name") == "ChromeInner") { inner = &event; }
        }
    }

    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);
    REQUIRE(threadNameEvent != nullptr);
    CHECK(threadNameEvents == 1);

    // Both zones belong to the thread the metadata event named.
    CHECK(outer->at("tid") == threadNameEvent->at("tid"));
    CHECK(inner->at("tid") == threadNameEvent->at("tid"));

    // The format requires microseconds, while VkmProfileZone stores nanoseconds. A 2 ms sleep
    // is at least 1000 us; deliberately no upper bound, so a slow machine cannot fail this.
    const double outerDurationUs = outer->at("dur").get<double>();
    CHECK(outerDurationUs >= 1000.0);

    // The inner scope is contained by the outer one, which is what makes the viewer nest them.
    const double outerBeginUs = outer->at("ts").get<double>();
    const double innerBeginUs = inner->at("ts").get<double>();
    CHECK(innerBeginUs >= outerBeginUs);
    CHECK(innerBeginUs + inner->at("dur").get<double>() <= outerBeginUs + outerDurationUs);

    fs::remove(tracePath);
    resetProfiler();
}
#endif // ENABLE_CHROME_TRACING
