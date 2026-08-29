// Copyright (c) 2026 Snowapril

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/gpu_profiler.h>

#include <filesystem>
#include <fstream>
#include <vector>

#if defined(ENABLE_CHROME_TRACING)
#include <nlohmann/json.hpp>
#endif

using namespace vkm;

/*
* The arithmetic that puts a GPU timestamp on the CPU clock, and the fallback that places a
* submission by its submit time where no clock pair is available. Both are free functions so
* they are exercisable from values built by hand, with no device in the way.
*/
namespace
{
    VkmGpuClockCalibration makeCalibration(const uint64_t cpuNs, const uint64_t gpuTicks,
                                           const double periodNs)
    {
        VkmGpuClockCalibration calibration;
        calibration._valid = true;
        calibration._cpuNs = cpuNs;
        calibration._gpuTicks = gpuTicks;
        calibration._timestampPeriodNs = periodNs;
        return calibration;
    }

    VkmGpuProfileZone makeZone(const char* name, const uint64_t beginNs, const uint64_t endNs,
                               const uint16_t depth = 0)
    {
        VkmGpuProfileZone zone;
        zone._name = name;
        zone._beginNs = beginNs;
        zone._endNs = endNs;
        zone._depth = depth;
        return zone;
    }
} // namespace

TEST_CASE("a GPU tick on the calibration's own anchor maps to the anchor's CPU time")
{
    const VkmGpuClockCalibration calibration = makeCalibration(1'000, 5'000, 1.0);
    CHECK(vkmGpuTicksToCpuNs(calibration, 5'000) == 1'000);
}

TEST_CASE("mapping applies both the offset and the tick period")
{
    // 24 MHz, the rate an Apple silicon counter heap ticks at.
    const VkmGpuClockCalibration calibration = makeCalibration(1'000'000, 100'000, 41.6667);

    // 240 ticks past the anchor is 240 * 41.6667 ns = 10000.008 ns later.
    CHECK(vkmGpuTicksToCpuNs(calibration, 100'240) == 1'010'000);
    // And the same distance before it maps symmetrically.
    CHECK(vkmGpuTicksToCpuNs(calibration, 99'760) == 990'000);
}

TEST_CASE("a GPU tick from before the CPU clock's epoch clamps rather than wrapping")
{
    // The epoch is taken on first use of VkmCpuProfiler::nowNs(), so a GPU timestamp older than
    // it maps below zero; an unsigned wrap would put the zone 584 years in the future.
    const VkmGpuClockCalibration calibration = makeCalibration(1'000, 500'000, 1.0);
    CHECK(vkmGpuTicksToCpuNs(calibration, 0) == 0);
    CHECK(vkmGpuTicksToCpuNs(calibration, 498'999) == 0);
    CHECK(vkmGpuTicksToCpuNs(calibration, 499'500) == 500);
}

TEST_CASE("mapping through an unsampled calibration reports nothing rather than guessing")
{
    const VkmGpuClockCalibration calibration; // never sampled
    CHECK(vkmGpuTicksToCpuNs(calibration, 123'456) == 0);
}

TEST_CASE("anchoring to a submit moves a submission without distorting it")
{
    std::vector<VkmGpuProfileZone> zones{
        makeZone("Frame", 10'000, 14'000, 0),
        makeZone("Shadow", 10'500, 11'500, 1),
        makeZone("Lighting", 12'000, 13'750, 1),
    };

    vkmAnchorGpuZonesToSubmit(zones, 1'000'000);

    // The earliest zone lands exactly on the submit, and everything keeps its offset from it.
    CHECK(zones[0]._beginNs == 1'000'000);
    CHECK(zones[0]._endNs == 1'004'000);
    CHECK(zones[1]._beginNs == 1'000'500);
    CHECK(zones[1]._endNs == 1'001'500);
    CHECK(zones[2]._beginNs == 1'002'000);
    CHECK(zones[2]._endNs == 1'003'750);

    // Durations and the gaps between zones are exactly what they were.
    CHECK(zones[0].getDurationNs() == 4'000);
    CHECK(zones[1].getDurationNs() == 1'000);
    CHECK(zones[2].getDurationNs() == 1'750);
    CHECK(zones[2]._beginNs - zones[1]._endNs == 500);
}

TEST_CASE("anchoring backwards past zero clamps instead of wrapping")
{
    std::vector<VkmGpuProfileZone> zones{ makeZone("Frame", 5'000, 6'000) };
    vkmAnchorGpuZonesToSubmit(zones, 0);
    CHECK(zones[0]._beginNs == 0);
    CHECK(zones[0]._endNs == 1'000);
}

TEST_CASE("anchoring an empty submission is a no-op")
{
    std::vector<VkmGpuProfileZone> zones;
    vkmAnchorGpuZonesToSubmit(zones, 1'000'000);
    CHECK(zones.empty());
}

TEST_CASE("a submit marker defaults to carrying no GPU work")
{
    // An upload or an acceleration structure build records no zones, and must still leave a
    // marker rather than vanishing from the timeline.
    const VkmGpuSubmitMarker marker;
    CHECK(marker._hasGpuWork == false);
    CHECK(marker._gpuBeginNs == 0);
}

TEST_CASE("a frame reports no correlation until something places its timestamps")
{
    const VkmGpuProfileFrame frame;
    CHECK(frame._correlation == VkmGpuClockCorrelation::None);
    // The window treats a weaker correlation as the frame's own, so the ordering matters.
    CHECK(VkmGpuClockCorrelation::None < VkmGpuClockCorrelation::Estimated);
    CHECK(VkmGpuClockCorrelation::Estimated < VkmGpuClockCorrelation::Calibrated);
}

#if defined(ENABLE_CHROME_TRACING)
/*
* The combined trace is the only artifact that carries both halves at once, since a viewer opens
* one file at a time. What matters is that the two land on separate process rows sharing one time
* axis, and that each timed submit links to the work it produced.
*/
TEST_CASE("the combined trace carries both halves and links each submit to its GPU work")
{
    VkmProfileThreadTimeline thread;
    thread._threadId = 77;
    thread._threadName = "MainThread";
    VkmProfileZone cpuZone;
    cpuZone._name = "RenderGraph::execute";
    cpuZone._beginNs = 1'000'000;
    cpuZone._endNs = 3'000'000;
    thread._zones.push_back(cpuZone);

    VkmProfileFrame cpuFrame;
    cpuFrame._frameNumber = 12;
    cpuFrame._beginNs = 1'000'000;
    cpuFrame._endNs = 3'000'000;
    cpuFrame._threads.push_back(thread);

    VkmGpuQueueTimeline queue;
    queue._queueType = VkmCommandQueueType::Graphics;
    queue._queueIndex = 0;
    queue._queueName = "MainGraphics";
    queue._zones.push_back(makeZone("Frame", 4'000'000, 6'000'000, 0));

    VkmGpuSubmitMarker marker;
    marker._beginNs = 2'900'000;
    marker._endNs = 3'000'000;
    marker._hasGpuWork = true;
    marker._gpuBeginNs = 4'000'000;
    queue._submits.push_back(marker);

    VkmGpuSubmitMarker untimed;  // an upload: a marker, but no zones to link to
    untimed._beginNs = 3'100'000;
    untimed._endNs = 3'150'000;
    queue._submits.push_back(untimed);

    VkmGpuProfileFrame gpuFrame;
    gpuFrame._frameNumber = 12;
    gpuFrame._beginNs = 4'000'000;
    gpuFrame._endNs = 6'000'000;
    gpuFrame._correlation = VkmGpuClockCorrelation::Calibrated;
    gpuFrame._queues.push_back(queue);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vkm_test_profile_trace.json";
    std::filesystem::remove(path);
    REQUIRE(vkmWriteProfileChromeTrace({ cpuFrame }, { gpuFrame }, path.string()));

    std::ifstream in(path);
    REQUIRE(in.is_open());
    const nlohmann::json trace = nlohmann::json::parse(in);
    in.close();
    const nlohmann::json& events = trace.at("traceEvents");

    int cpuZones = 0;
    int gpuZones = 0;
    int submits = 0;
    int flowStarts = 0;
    int flowEnds = 0;
    double cpuZoneTs = 0.0;
    double gpuZoneTs = 0.0;
    double flowStartTs = 0.0;
    double flowEndTs = 0.0;
    bool sawCpuThreadName = false;
    bool sawQueueName = false;

    for (const nlohmann::json& event : events)
    {
        const std::string phase = event.at("ph").get<std::string>();
        if (phase == "M")
        {
            const std::string name = event.at("args").at("name").get<std::string>();
            sawCpuThreadName = sawCpuThreadName || (name == "MainThread");
            sawQueueName = sawQueueName || (name == "MainGraphics");
        }
        else if (phase == "X")
        {
            const std::string category = event.at("cat").get<std::string>();
            if (category == "cpu")
            {
                ++cpuZones;
                cpuZoneTs = event.at("ts").get<double>();
                CHECK(event.at("pid").get<int>() == 1);
            }
            else if (category == "gpu")
            {
                ++gpuZones;
                gpuZoneTs = event.at("ts").get<double>();
                CHECK(event.at("pid").get<int>() == 2);
            }
            else if (category == "submit")
            {
                ++submits;
                CHECK(event.at("pid").get<int>() == 2);
            }
        }
        else if (phase == "s")
        {
            ++flowStarts;
            flowStartTs = event.at("ts").get<double>();
        }
        else if (phase == "f")
        {
            ++flowEnds;
            flowEndTs = event.at("ts").get<double>();
        }
    }

    CHECK(sawCpuThreadName);
    CHECK(sawQueueName);
    CHECK(cpuZones == 1);
    CHECK(gpuZones == 1);
    // Both submits are drawn, including the one with no GPU work behind it.
    CHECK(submits == 2);
    // Only the timed one gets an arrow.
    CHECK(flowStarts == 1);
    CHECK(flowEnds == 1);

    // Microseconds, as the format requires, and the correlation is visible in the file: the GPU
    // work starts after the CPU scope that submitted it, and the arrow spans exactly the wait.
    CHECK(cpuZoneTs == doctest::Approx(1000.0));
    CHECK(gpuZoneTs == doctest::Approx(4000.0));
    CHECK(gpuZoneTs > cpuZoneTs);
    CHECK(flowStartTs == doctest::Approx(3000.0));
    CHECK(flowEndTs == doctest::Approx(4000.0));

    std::filesystem::remove(path);
}
#endif // ENABLE_CHROME_TRACING
