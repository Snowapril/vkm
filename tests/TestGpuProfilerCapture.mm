#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/gpu_profiler.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/texture.h>

#include <vector>

static constexpr int kWidth  = 64;
static constexpr int kHeight = 64;

// MTLCreateSystemDefaultDevice() is called solely to construct VkmDriverMetal. No raw Metal work
// is driven from this fixture -- every GPU operation goes through VkmRenderGraph.
struct GpuProfilerFixture {
    vkm::VkmDriverMetal* driver = nullptr;
    vkm::VkmInitResult initResult;

    GpuProfilerFixture() {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported, "No Metal device available on this system."};
            return;
        }
        vkm::VkmEngineLaunchOptions opts = { .enableValidationLayer = true };
        driver = new vkm::VkmDriverMetal(device);
        initResult = driver->initialize(&opts);
    }
    ~GpuProfilerFixture() {
        delete driver;
    }
};

namespace {
    const vkm::VkmGpuProfileZone* findZone(const vkm::VkmGpuQueueTimeline& timeline, const char* name) {
        for (const vkm::VkmGpuProfileZone& zone : timeline._zones) {
            if (zone._name != nullptr && std::string(zone._name) == name) {
                return &zone;
            }
        }
        return nullptr;
    }
} // namespace

TEST_CASE("GPU profiler times each render graph subgraph on the queue that ran it") {
    GpuProfilerFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver;

    vkm::VkmGpuProfiler* profiler = driver->getGpuProfiler();
    REQUIRE(profiler != nullptr);
    if (!profiler->isSupported()) {
        // Timestamp queries are a device capability, not a guarantee -- skip rather than fail.
        MESSAGE("GPU timestamp queries unavailable on this device; skipping");
        return;
    }

    vkm::VkmTextureInfo texInfo{};
    texInfo._flags          = vkm::VkmResourceCreateInfo::AllowColorAttachment;
    texInfo._extent         = glm::uvec3(kWidth, kHeight, 1);
    texInfo._format         = vkm::VkmFormat::R8G8B8A8_UNORM;
    texInfo._numMipLevels   = 1;
    texInfo._numArrayLayers = 1;
    texInfo._debugName      = "GpuProfilerTestOffscreen";
    vkm::VkmTexture* offscreen = driver->newTexture(texInfo);
    REQUIRE(offscreen != nullptr);

    vkm::VkmFrameBufferDescriptor fbDesc{};
    fbDesc._width  = static_cast<uint32_t>(kWidth);
    fbDesc._height = static_cast<uint32_t>(kHeight);
    fbDesc._renderPass._colorAttachmentCount               = 1;
    fbDesc._renderPass._colorAttachments[0]._attachmentId  = 0;
    fbDesc._renderPass._colorAttachments[0]._loadAction    = vkm::VkmLoadAction::Clear;
    fbDesc._renderPass._colorAttachments[0]._storeAction   = vkm::VkmStoreAction::Store;
    fbDesc._colorAttachments[0] = offscreen->getHandle();

    profiler->setCapturing(true);

    // Two subgraphs so the timeline has something to separate, and several frames so at least one
    // submission has completed and been retired by the time the checks run (collect() only reads
    // submissions the GPU has already finished).
    constexpr int kFrameCount = 4;
    for (int frameIndex = 0; frameIndex < kFrameCount; ++frameIndex) {
        profiler->collect();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        renderGraph.beginGraphicsSubGraph(fbDesc, "ProfilerTestPassA");
        renderGraph.beginGraphicsSubGraph(fbDesc, "ProfilerTestPassB");
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();
    }
    profiler->collect();

    REQUIRE(profiler->getFrameCount() > 0);

    // The most recently retired frame is the last one in the ring.
    vkm::VkmGpuProfileFrame frame;
    REQUIRE(profiler->copyFrame(profiler->getFrameCount() - 1, frame));

    // VkmRenderGraph::execute() only ever submits to Graphics queue 0 today, so exactly one row.
    REQUIRE(frame._queues.size() == 1);
    const vkm::VkmGpuQueueTimeline& timeline = frame._queues[0];
    CHECK(timeline._queueType == vkm::VkmCommandQueueType::Graphics);
    CHECK(timeline._queueIndex == 0);
    CHECK(timeline._queueName == "MainGraphics");
    CHECK_FALSE(timeline._overflowed);

    const vkm::VkmGpuProfileZone* submissionZone = findZone(timeline, "Frame");
    const vkm::VkmGpuProfileZone* passA = findZone(timeline, "ProfilerTestPassA");
    const vkm::VkmGpuProfileZone* passB = findZone(timeline, "ProfilerTestPassB");
    REQUIRE(submissionZone != nullptr);
    REQUIRE(passA != nullptr);
    REQUIRE(passB != nullptr);

    CHECK(submissionZone->_depth == 0);
    CHECK(passA->_depth == 1);
    CHECK(passB->_depth == 1);
    CHECK(passA->_subGraphId == 0);
    CHECK(passB->_subGraphId == 1);

    // Real elapsed GPU time, and each subgraph contained by the submission that recorded it.
    CHECK(submissionZone->getDurationNs() > 0);
    CHECK(passA->_beginNs >= submissionZone->_beginNs);
    CHECK(passB->_endNs <= submissionZone->_endNs);
    CHECK(passA->_beginNs <= passB->_beginNs); // committed in insertion order

    // The frame spans its zones, and the overlay's stat reflects the same submission.
    CHECK(frame._beginNs <= submissionZone->_beginNs);
    CHECK(frame._endNs >= submissionZone->_endNs);
    CHECK(profiler->getLastFrameGpuTimeMs() > 0.0);

    // Zones are handed to the chart in (begin, depth) order, parent first.
    for (size_t i = 1; i < timeline._zones.size(); ++i) {
        CHECK(timeline._zones[i - 1]._beginNs <= timeline._zones[i]._beginNs);
    }

    profiler->clear();
    CHECK(profiler->getFrameCount() == 0);
    vkm::VkmGpuProfileFrame outOfRange;
    CHECK_FALSE(profiler->copyFrame(0, outOfRange));

    profiler->setCapturing(false);
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
