// Copyright (c) 2025 Snowapril

#include <doctest/doctest.h>

#include <vkm/base/cpu_profiler.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

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
