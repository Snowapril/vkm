#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_graph_capture.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/driver.h>

#include <cstring>
#include <numeric>
#include <vector>

static constexpr int kWidth  = 64;
static constexpr int kHeight = 64;

// MTLCreateSystemDefaultDevice() is called solely to construct VkmDriverMetal.
// No raw Metal rendering work is done in or from this fixture.
struct CaptureFixture {
    vkm::VkmDriverMetal* driver = nullptr;
    vkm::VkmInitResult initResult;

    explicit CaptureFixture(vkm::VkmEngineLaunchOptions opts = { .enableValidationLayer = true }) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported, "No Metal device available on this system."};
            return;
        }
        driver = new vkm::VkmDriverMetal(device);
        initResult = driver->initialize(&opts);
    }
    ~CaptureFixture() {
        delete driver;
    }
};

TEST_CASE("Render graph capture - clear pass metadata, snapshot pixels, and buffer contents") {
    CaptureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver;

    // Offscreen color attachment cleared to solid blue by the captured pass.
    vkm::VkmTextureInfo texInfo{};
    texInfo._flags          = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                              vkm::VkmResourceCreateInfo::AllowTransferSrc;
    texInfo._extent         = glm::uvec3(kWidth, kHeight, 1);
    texInfo._format         = vkm::VkmFormat::R8G8B8A8_UNORM;
    texInfo._numMipLevels   = 1;
    texInfo._numArrayLayers = 1;
    texInfo._debugName      = "CaptureTestOffscreen";
    vkm::VkmTexture* offscreen = driver->newTexture(texInfo);
    REQUIRE(offscreen != nullptr);

    // A buffer with known contents, referenced by the pass so the capture reads it back.
    std::vector<uint8_t> bufferContents(256);
    std::iota(bufferContents.begin(), bufferContents.end(), 0);
    vkm::VkmBufferInfo bufferInfo{};
    bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                        vkm::VkmResourceCreateInfo::AllowTransferSrc |
                        vkm::VkmResourceCreateInfo::AllowTransferDst;
    bufferInfo._size = bufferContents.size();
    bufferInfo._debugName = "CaptureTestBuffer";
    vkm::VkmBuffer* buffer = driver->newBuffer(bufferInfo);
    REQUIRE(buffer != nullptr);
    REQUIRE(driver->uploadToBuffer(buffer->getHandle(), bufferContents.data(), bufferContents.size()));

    vkm::VkmFrameBufferDescriptor fbDesc{};
    fbDesc._width  = static_cast<uint32_t>(kWidth);
    fbDesc._height = static_cast<uint32_t>(kHeight);
    fbDesc._renderPass._colorAttachmentCount                 = 1;
    fbDesc._renderPass._colorAttachments[0]._attachmentId   = 0;
    fbDesc._renderPass._colorAttachments[0]._loadAction     = vkm::VkmLoadAction::Clear;
    fbDesc._renderPass._colorAttachments[0]._storeAction    = vkm::VkmStoreAction::Store;
    fbDesc._renderPass._colorAttachments[0]._clearColors[0] = 0.0f;
    fbDesc._renderPass._colorAttachments[0]._clearColors[1] = 0.0f;
    fbDesc._renderPass._colorAttachments[0]._clearColors[2] = 1.0f;
    fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
    fbDesc._colorAttachments[0] = offscreen->getHandle();

    vkm::VkmRenderGraphCapture capture;
    CHECK(capture.getState() == vkm::VkmRenderGraphCapture::State::Idle);
    capture.arm();
    CHECK(capture.getState() == vkm::VkmRenderGraphCapture::State::Armed);

    vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
    vkm::VkmRenderGraphicsSubGraph* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc, "CaptureTestPass");
    subGraph->addReferencedResource(buffer->getHandle());
    renderGraph.compile();
    renderGraph.execute(vkm::VkmRenderGraphCommitOptions{ .capture = &capture });
    renderGraph.ensureCompleted();
    capture.finalize(driver);

    REQUIRE(capture.getState() == vkm::VkmRenderGraphCapture::State::Ready);
    CHECK(capture.hasContentCapture());
    REQUIRE(capture.getPasses().size() == 1);

    const vkm::VkmCapturedPass& pass = capture.getPasses()[0];
    CHECK(pass.name == "CaptureTestPass");
    CHECK(pass.type == vkm::VkmRenderSubGraphType::Graphics);
    CHECK(pass.width == kWidth);
    CHECK(pass.height == kHeight);
    REQUIRE(pass.colorAttachments.size() == 1);

    const vkm::VkmCapturedAttachment& attachment = pass.colorAttachments[0];
    CHECK(attachment.loadAction == vkm::VkmLoadAction::Clear);
    CHECK(attachment.storeAction == vkm::VkmStoreAction::Store);
    CHECK(attachment.info.format == vkm::VkmFormat::R8G8B8A8_UNORM);
    CHECK(attachment.info.extent.x == kWidth);
    CHECK(attachment.info.extent.y == kHeight);
    CHECK(attachment.info.debugName == "CaptureTestOffscreen");
    CHECK_FALSE(attachment.isPresentTarget);
    REQUIRE(attachment.snapshotTexture.isValid());

    // The snapshot must contain the pass's output: solid blue.
    vkm::VkmTextureReadbackResult snapshotPixels = driver->readbackTexture(attachment.snapshotTexture);
    REQUIRE(snapshotPixels.pixels.size() == (size_t)kWidth * kHeight * 4);
    for (size_t i = 0; i < snapshotPixels.pixels.size(); i += 4)
    {
        REQUIRE(snapshotPixels.pixels[i + 0] == 0);   // R
        REQUIRE(snapshotPixels.pixels[i + 1] == 0);   // G
        REQUIRE(snapshotPixels.pixels[i + 2] == 255); // B
        REQUIRE(snapshotPixels.pixels[i + 3] == 255); // A
    }

    // The referenced buffer's contents must have been read back verbatim.
    REQUIRE(pass.capturedBuffers.size() == 1);
    const vkm::VkmCapturedBuffer& capturedBuffer = pass.capturedBuffers[0];
    CHECK(capturedBuffer.info.debugName == "CaptureTestBuffer");
    CHECK(capturedBuffer.info.size == bufferContents.size());
    REQUIRE(capturedBuffer.data.size() == bufferContents.size());
    CHECK(std::memcmp(capturedBuffer.data.data(), bufferContents.data(), bufferContents.size()) == 0);

    // The buffer must also appear in the pass's referenced-resource (inputs) list.
    REQUIRE(pass.referencedResources.size() == 1);
    CHECK(pass.referencedResources[0].debugName == "CaptureTestBuffer");

    capture.releaseResources(driver);
    CHECK(capture.getState() == vkm::VkmRenderGraphCapture::State::Idle);
}

#if defined(VKM_GPU_CAPTURE)
TEST_CASE("GPU frame capture - scope hooks and request are crash-free headless") {
    // With enableGpuCapture set, the driver creates a frame-aligned MTLCaptureScope.
    // Headless (MTL_CAPTURE_ENABLED unset), a requested .gputrace capture exercises the
    // supportsDestination failure path gracefully; scope begin/end with no commits in
    // between is validation-legal. Real .gputrace output is verified manually.
    CaptureFixture f(vkm::VkmEngineLaunchOptions{ .enableValidationLayer = true, .enableGpuCapture = true });
    VKM_REQUIRE_DEVICE(f.initResult);

    // Delayed multi-frame request: start 1 frame later, span 2 frames. Run enough
    // frame-boundary pairs to cover the delay, the capture window, and one idle frame.
    f.driver->requestGpuFrameCapture(/*startFrameDelay=*/1, /*frameCount=*/2);
    for (int i = 0; i < 4; ++i)
    {
        f.driver->onFrameBegin();
        f.driver->onFrameEnd();
    }
}
#endif // VKM_GPU_CAPTURE

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
