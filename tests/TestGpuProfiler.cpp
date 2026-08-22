// Copyright (c) 2026 Snowapril

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/gpu_profiler.h>

#include <string>
#include <vector>

#if defined(ENABLE_CHROME_TRACING)
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
namespace fs = std::filesystem;
#endif // ENABLE_CHROME_TRACING

/*
* Range aggregation and the trace format are pure functions over a VkmGpuProfileFrame, so these
* cases build frames by hand rather than driving a real capture -- the expected nanosecond totals
* are then exact, and none of it needs a device. The recording path itself (timestamp slots,
* submission retirement, the frame ring) is covered by TestGpuProfilerCapture.mm, which needs one.
*/
namespace
{
    vkm::VkmGpuProfileZone makeZone(const char* name, uint64_t beginNs, uint64_t endNs,
                                    uint16_t depth = 0, uint32_t subGraphId = vkm::INVALID_VALUE32)
    {
        vkm::VkmGpuProfileZone zone;
        zone._name = name;
        zone._beginNs = beginNs;
        zone._endNs = endNs;
        zone._depth = depth;
        zone._subGraphId = subGraphId;
        return zone;
    }

    vkm::VkmGpuQueueTimeline makeTimeline(vkm::VkmCommandQueueType queueType, uint32_t queueIndex,
                                          const char* queueName,
                                          std::vector<vkm::VkmGpuProfileZone> zones)
    {
        vkm::VkmGpuQueueTimeline timeline;
        timeline._queueType = queueType;
        timeline._queueIndex = queueIndex;
        timeline._queueName = queueName;
        timeline._zones = std::move(zones);
        return timeline;
    }

    vkm::VkmGpuProfileFrame makeFrame(std::vector<vkm::VkmGpuQueueTimeline> queues,
                                      uint32_t frameNumber = 0, uint64_t beginNs = 0,
                                      uint64_t endNs = 1000)
    {
        vkm::VkmGpuProfileFrame frame;
        frame._frameNumber = frameNumber;
        frame._beginNs = beginNs;
        frame._endNs = endNs;
        frame._queues = std::move(queues);
        return frame;
    }

    const vkm::VkmGpuProfileZoneTotal* findTotal(const std::vector<vkm::VkmGpuProfileZoneTotal>& totals,
                                                 const char* name)
    {
        for (const vkm::VkmGpuProfileZoneTotal& entry : totals)
        {
            if (std::string(entry._name) == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }
} // namespace

TEST_CASE("GPU range aggregation clips zones to the range instead of counting them whole")
{
    const vkm::VkmGpuQueueTimeline timeline = makeTimeline(
        vkm::VkmCommandQueueType::Graphics, 0, "MainGraphics",
        {
            makeZone("Straddles", 50, 250),  // only 150 of its 200 ns is inside
            makeZone("Inside", 120, 170),    // wholly inside
            makeZone("Outside", 300, 400),   // wholly outside
        });

    const std::vector<vkm::VkmGpuProfileZoneTotal> totals =
        vkm::vkmAggregateGpuProfileRange(makeFrame({ timeline }), 100, 250);

    REQUIRE(totals.size() == 2); // "Outside" contributes nothing and gets no row

    const vkm::VkmGpuProfileZoneTotal* straddles = findTotal(totals, "Straddles");
    REQUIRE(straddles != nullptr);
    CHECK(straddles->_totalNs == 150);
    CHECK(straddles->_count == 1);

    const vkm::VkmGpuProfileZoneTotal* inside = findTotal(totals, "Inside");
    REQUIRE(inside != nullptr);
    CHECK(inside->_totalNs == 50);

    CHECK(findTotal(totals, "Outside") == nullptr);

    // Longest first, which is what makes the top of the table the answer.
    CHECK(totals[0]._totalNs >= totals[1]._totalNs);
}

TEST_CASE("GPU range aggregation counts the submission zone and its subgraphs, and merges repeats")
{
    const vkm::VkmGpuQueueTimeline timeline = makeTimeline(
        vkm::VkmCommandQueueType::Graphics, 0, "MainGraphics",
        {
            makeZone("Frame", 0, 100, 0),
            makeZone("ScenePass", 10, 40, 1, 0),
            makeZone("ScenePass", 60, 90, 1, 1), // same name again: one row, two subgraphs
        });

    const std::vector<vkm::VkmGpuProfileZoneTotal> totals =
        vkm::vkmAggregateGpuProfileRange(makeFrame({ timeline }), 0, 100);

    const vkm::VkmGpuProfileZoneTotal* pass = findTotal(totals, "ScenePass");
    REQUIRE(pass != nullptr);
    CHECK(pass->_totalNs == 60); // 30 + 30
    CHECK(pass->_count == 2);

    const vkm::VkmGpuProfileZoneTotal* frame = findTotal(totals, "Frame");
    REQUIRE(frame != nullptr);
    CHECK(frame->_totalNs == 100); // the submission zone is not reduced by the subgraphs inside it

    // Nesting means the totals exceed the range's own duration; that is the documented shape.
    CHECK(frame->_totalNs + pass->_totalNs > 100);
}

TEST_CASE("GPU range aggregation sums across command queues and rejects an empty range")
{
    const vkm::VkmGpuProfileFrame frame = makeFrame({
        makeTimeline(vkm::VkmCommandQueueType::Graphics, 0, "MainGraphics", { makeZone("Shared", 0, 50) }),
        makeTimeline(vkm::VkmCommandQueueType::Compute, 0, "AsyncCompute", { makeZone("Shared", 0, 30) }),
    });

    const std::vector<vkm::VkmGpuProfileZoneTotal> totals = vkm::vkmAggregateGpuProfileRange(frame, 0, 100);
    REQUIRE(totals.size() == 1);
    CHECK(totals[0]._totalNs == 80);
    CHECK(totals[0]._count == 2);

    // A zero-width or inverted range has nothing to report rather than everything.
    CHECK(vkm::vkmAggregateGpuProfileRange(frame, 50, 50).empty());
    CHECK(vkm::vkmAggregateGpuProfileRange(frame, 80, 20).empty());
}

TEST_CASE("Subgraph averages mean the samples of every depth-1 zone naming a subgraph")
{
    vkm::VkmGpuSubGraphAverages averages;

    // The submission-wide zone and a depth-1 zone that names no subgraph are both ignored, so
    // only "GBuffer" is measured: 200 ns and 400 ns, a 300 ns mean.
    averages.addZones({
        makeZone("Frame", 0, 5000, 0),
        makeZone("GBuffer", 100, 300, 1, 3),
        makeZone("Unattributed", 300, 900, 1),
    });
    averages.addZones({ makeZone("GBuffer", 100, 500, 1, 3) });

    CHECK(averages.getSampleCount("GBuffer") == 2);
    CHECK(averages.getAverageMs("GBuffer") == doctest::Approx(300.0 * 1e-6));
    CHECK(averages.getSampleCount("Frame") == 0);
    CHECK(averages.getSampleCount("Unattributed") == 0);

    // A name that never ran reads as no data rather than as zero milliseconds spent.
    CHECK(averages.getSampleCount("Missing") == 0);
    CHECK(averages.getAverageMs("Missing") == doctest::Approx(0.0));

    averages.clear();
    CHECK(averages.getSampleCount("GBuffer") == 0);
}

TEST_CASE("Subgraph averages drop the oldest sample once the window is full")
{
    vkm::VkmGpuSubGraphAverages averages;

    // Fill the window with 1000 ns samples, then push the same number of 2000 ns ones: the mean
    // has to end up at 2000 ns exactly, which only holds if every old sample left the window.
    for (size_t i = 0; i < vkm::VkmGpuSubGraphAverages::kWindowSize; ++i)
    {
        averages.addZones({ makeZone("Lighting", 0, 1000, 1, 4) });
    }
    CHECK(averages.getSampleCount("Lighting") == vkm::VkmGpuSubGraphAverages::kWindowSize);
    CHECK(averages.getAverageMs("Lighting") == doctest::Approx(1000.0 * 1e-6));

    for (size_t i = 0; i < vkm::VkmGpuSubGraphAverages::kWindowSize; ++i)
    {
        averages.addZones({ makeZone("Lighting", 0, 2000, 1, 4) });
    }
    CHECK(averages.getSampleCount("Lighting") == vkm::VkmGpuSubGraphAverages::kWindowSize);
    CHECK(averages.getAverageMs("Lighting") == doctest::Approx(2000.0 * 1e-6));
}

#if defined(ENABLE_CHROME_TRACING)
TEST_CASE("vkmWriteGpuChromeTrace emits microsecond complete events and one row per command queue")
{
    // 1 ms and 0.4 ms in nanoseconds, so the microsecond conversion is checkable exactly.
    const std::vector<vkm::VkmGpuProfileFrame> frames = {
        makeFrame(
            {
                makeTimeline(vkm::VkmCommandQueueType::Graphics, 0, "MainGraphics",
                             {
                                 makeZone("Frame", 5'000'000, 6'000'000, 0),
                                 makeZone("ScenePass", 5'200'000, 5'600'000, 1, 0),
                             }),
                makeTimeline(vkm::VkmCommandQueueType::Compute, 0, "AsyncCompute",
                             { makeZone("SceneCull", 5'100'000, 5'300'000, 0) }),
            },
            /*frameNumber=*/7, /*beginNs=*/5'000'000, /*endNs=*/6'000'000),
    };

    const fs::path tracePath = fs::temp_directory_path() / "vkm_test_gpu_chrome_trace.json";
    REQUIRE(vkm::vkmWriteGpuChromeTrace(frames, tracePath.string()));

    std::ifstream in(tracePath);
    REQUIRE(in.good());
    const nlohmann::json trace = nlohmann::json::parse(in);
    in.close();

    REQUIRE(trace.contains("traceEvents"));
    const nlohmann::json& events = trace["traceEvents"];

    // One metadata event per queue, carrying the queue's own name -- that is what makes the
    // viewer label the rows "MainGraphics" / "AsyncCompute" instead of numbering them.
    std::vector<std::string> queueRowNames;
    int graphicsRowId = -1;
    int computeRowId = -1;
    for (const nlohmann::json& event : events)
    {
        if (event.value("ph", "") != "M" || event.value("name", "") != "thread_name")
        {
            continue;
        }
        const std::string queueName = event["args"]["name"].get<std::string>();
        queueRowNames.push_back(queueName);
        if (queueName == "MainGraphics") { graphicsRowId = event["tid"].get<int>(); }
        if (queueName == "AsyncCompute") { computeRowId = event["tid"].get<int>(); }
    }
    REQUIRE(queueRowNames.size() == 2);
    REQUIRE(graphicsRowId >= 0);
    REQUIRE(computeRowId >= 0);
    CHECK(graphicsRowId != computeRowId); // separate rows, which is the whole point of per-queue

    const nlohmann::json* frameEvent = nullptr;
    const nlohmann::json* passEvent = nullptr;
    const nlohmann::json* cullEvent = nullptr;
    for (const nlohmann::json& event : events)
    {
        if (event.value("ph", "") != "X")
        {
            continue;
        }
        CHECK(event.value("cat", "") == "gpu");
        const std::string name = event.value("name", "");
        if (name == "Frame")     { frameEvent = &event; }
        if (name == "ScenePass") { passEvent = &event; }
        if (name == "SceneCull") { cullEvent = &event; }
    }
    REQUIRE(frameEvent != nullptr);
    REQUIRE(passEvent != nullptr);
    REQUIRE(cullEvent != nullptr);

    // Microseconds, not the nanoseconds the zones store.
    CHECK((*frameEvent)["ts"].get<double>() == doctest::Approx(5000.0));
    CHECK((*frameEvent)["dur"].get<double>() == doctest::Approx(1000.0));
    CHECK((*passEvent)["dur"].get<double>() == doctest::Approx(400.0));

    // The subgraph rides the graphics row and is contained by the submission zone in
    // ts + dur, which is what makes the viewer nest them.
    CHECK((*frameEvent)["tid"].get<int>() == graphicsRowId);
    CHECK((*passEvent)["tid"].get<int>() == graphicsRowId);
    CHECK((*cullEvent)["tid"].get<int>() == computeRowId);
    CHECK((*passEvent)["ts"].get<double>() >= (*frameEvent)["ts"].get<double>());
    CHECK((*passEvent)["ts"].get<double>() + (*passEvent)["dur"].get<double>() <=
          (*frameEvent)["ts"].get<double>() + (*frameEvent)["dur"].get<double>());

    // Both processes' rows must not collide when the CPU and GPU traces are loaded together;
    // the CPU export uses pid 1.
    CHECK((*frameEvent)["pid"].get<int>() != 1);

    fs::remove(tracePath);
}
#endif // ENABLE_CHROME_TRACING
