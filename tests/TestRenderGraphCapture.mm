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

#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#include <algorithm>
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
    subGraph->addReferencedResource(buffer->getHandle(), vkm::VkmResourceAccess::ShaderStorageRead);
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
    CHECK(attachment.info.access == vkm::VkmResourceAccess::ColorAttachmentWrite);
    CHECK_FALSE(attachment.isPresentTarget);
    REQUIRE(attachment.info.snapshotTexture.isValid());

    // The snapshot must contain the pass's output: solid blue.
    vkm::VkmTextureReadbackResult snapshotPixels = driver->readbackTexture(attachment.info.snapshotTexture);
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

namespace
{
    // A pass that clears an offscreen target and declares `input` as a referenced resource,
    // captured end to end. Returns the captured pass so each case can assert on the input.
    struct InputCaptureRun
    {
        vkm::VkmRenderGraphCapture capture;
        vkm::VkmTexture* offscreen = nullptr;
    };

    void runCaptureWithTextureInput(vkm::VkmDriverBase* driver, InputCaptureRun& run,
                                    vkm::VkmResourceHandle input)
    {
        vkm::VkmTextureInfo texInfo{};
        texInfo._flags          = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                                  vkm::VkmResourceCreateInfo::AllowTransferSrc;
        texInfo._extent         = glm::uvec3(kWidth, kHeight, 1);
        texInfo._format         = vkm::VkmFormat::R8G8B8A8_UNORM;
        texInfo._numMipLevels   = 1;
        texInfo._numArrayLayers = 1;
        texInfo._debugName      = "InputCaptureOffscreen";
        run.offscreen = driver->newTexture(texInfo);
        REQUIRE(run.offscreen != nullptr);

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width  = static_cast<uint32_t>(kWidth);
        fbDesc._height = static_cast<uint32_t>(kHeight);
        fbDesc._renderPass._colorAttachmentCount               = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction   = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction  = vkm::VkmStoreAction::Store;
        fbDesc._colorAttachments[0] = run.offscreen->getHandle();

        run.capture.arm();
        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        vkm::VkmRenderGraphicsSubGraph* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc, "InputCapturePass");
        subGraph->addReferencedResource(input, vkm::VkmResourceAccess::ShaderSampledRead);
        renderGraph.compile();
        renderGraph.execute(vkm::VkmRenderGraphCommitOptions{ .capture = &run.capture });
        renderGraph.ensureCompleted();
        run.capture.finalize(driver);
    }
} // namespace

TEST_CASE("Render graph capture - a referenced 2D input texture is snapshotted with its contents") {
    CaptureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver;

    // A plain sampled texture: AllowShaderRead|AllowTransferDst and no AllowTransferSrc, the
    // shape every loaded asset has. The capture must still snapshot it.
    vkm::VkmTextureInfo inputInfo{};
    inputInfo._flags          = vkm::VkmResourceCreateInfo::AllowShaderRead |
                                vkm::VkmResourceCreateInfo::AllowTransferDst;
    inputInfo._extent         = glm::uvec3(kWidth, kHeight, 1);
    inputInfo._format         = vkm::VkmFormat::R8G8B8A8_UNORM;
    inputInfo._numMipLevels   = 1;
    inputInfo._numArrayLayers = 1;
    inputInfo._debugName      = "CaptureTestInput";
    vkm::VkmTexture* input = driver->newTexture(inputInfo);
    REQUIRE(input != nullptr);

    std::vector<uint8_t> pixels((size_t)kWidth * kHeight * 4);
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i + 0] = 12; pixels[i + 1] = 34; pixels[i + 2] = 56; pixels[i + 3] = 255;
    }
    REQUIRE(driver->uploadToTexture(input->getHandle(), pixels.data(), pixels.size()));

    InputCaptureRun run;
    runCaptureWithTextureInput(driver, run, input->getHandle());

    REQUIRE(run.capture.getState() == vkm::VkmRenderGraphCapture::State::Ready);
    REQUIRE(run.capture.getPasses().size() == 1);
    const vkm::VkmCapturedPass& pass = run.capture.getPasses()[0];
    REQUIRE(pass.referencedResources.size() == 1);

    const vkm::VkmCapturedResourceInfo& info = pass.referencedResources[0];
    CHECK(info.type == vkm::VkmResourceType::Texture);
    CHECK(info.debugName == "CaptureTestInput");
    CHECK(info.format == vkm::VkmFormat::R8G8B8A8_UNORM);
    CHECK(info.numArrayLayers == 1);
    REQUIRE(info.snapshotTexture.isValid());

    vkm::VkmTextureReadbackResult snapshot = driver->readbackTexture(info.snapshotTexture);
    REQUIRE(snapshot.pixels.size() == pixels.size());
    CHECK(std::memcmp(snapshot.pixels.data(), pixels.data(), pixels.size()) == 0);

    run.capture.releaseResources(driver);
}

TEST_CASE("Render graph capture - a cube input is snapshotted as face 0, validation-clean") {
    CaptureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver;

    vkm::VkmTextureInfo cubeInfo{};
    cubeInfo._flags          = vkm::VkmResourceCreateInfo::AllowShaderRead |
                               vkm::VkmResourceCreateInfo::AllowTransferDst;
    cubeInfo._extent         = glm::uvec3(kWidth, kWidth, 1);
    cubeInfo._format         = vkm::VkmFormat::R8G8B8A8_UNORM;
    cubeInfo._numMipLevels   = 1;
    cubeInfo._numArrayLayers = 6;
    cubeInfo._type           = vkm::VkmTextureType::Cube;
    cubeInfo._debugName      = "CaptureTestCube";
    vkm::VkmTexture* cube = driver->newTexture(cubeInfo);
    REQUIRE(cube != nullptr);

    // Distinct color per face so a face-0 snapshot cannot pass by accident.
    std::vector<uint8_t> facePixels((size_t)kWidth * kWidth * 4);
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (size_t i = 0; i < facePixels.size(); i += 4)
        {
            facePixels[i + 0] = static_cast<uint8_t>(face * 40);
            facePixels[i + 1] = 7;
            facePixels[i + 2] = 9;
            facePixels[i + 3] = 255;
        }
        REQUIRE(driver->uploadToTexture(cube->getHandle(), facePixels.data(), facePixels.size(), 0, face));
    }

    InputCaptureRun run;
    runCaptureWithTextureInput(driver, run, cube->getHandle());

    REQUIRE(run.capture.getPasses().size() == 1);
    const vkm::VkmCapturedResourceInfo& info = run.capture.getPasses()[0].referencedResources[0];
    CHECK(info.numArrayLayers == 6);
    CHECK(info.textureType == vkm::VkmTextureType::Cube);
    REQUIRE(info.snapshotTexture.isValid());

    // copyTexture is defined as mip 0 / layer 0, so the snapshot must hold face 0's color.
    vkm::VkmTextureReadbackResult snapshot = driver->readbackTexture(info.snapshotTexture);
    REQUIRE(snapshot.pixels.size() == facePixels.size());
    CHECK(snapshot.pixels[0] == 0);
    CHECK(snapshot.pixels[1] == 7);
    CHECK(snapshot.pixels[2] == 9);

    run.capture.releaseResources(driver);
}

TEST_CASE("Render graph capture - a depth input records metadata but no snapshot") {
    CaptureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver;

    vkm::VkmTextureInfo depthInfo{};
    depthInfo._flags          = vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment |
                                vkm::VkmResourceCreateInfo::AllowShaderRead;
    depthInfo._extent         = glm::uvec3(kWidth, kHeight, 1);
    depthInfo._format         = vkm::VkmFormat::D32_SFLOAT;
    depthInfo._numMipLevels   = 1;
    depthInfo._numArrayLayers = 1;
    depthInfo._debugName      = "CaptureTestDepthInput";
    vkm::VkmTexture* depth = driver->newTexture(depthInfo);
    REQUIRE(depth != nullptr);

    InputCaptureRun run;
    runCaptureWithTextureInput(driver, run, depth->getHandle());

    REQUIRE(run.capture.getPasses().size() == 1);
    const vkm::VkmCapturedResourceInfo& info = run.capture.getPasses()[0].referencedResources[0];
    CHECK(info.debugName == "CaptureTestDepthInput");
    CHECK(info.format == vkm::VkmFormat::D32_SFLOAT);
    CHECK(info.extent.x == kWidth);
    CHECK_FALSE(info.snapshotTexture.isValid());

    run.capture.releaseResources(driver);
}

TEST_CASE("Resource pool - getAllResourceHandles reports live textures only") {
    CaptureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver;
    vkm::VkmRenderResourcePool* pool = driver->getRenderResourcePool();

    const size_t before = pool->getAllResourceHandles(vkm::VkmResourceType::Texture).size();

    vkm::VkmTextureInfo info{};
    info._flags          = vkm::VkmResourceCreateInfo::AllowShaderRead |
                           vkm::VkmResourceCreateInfo::AllowTransferDst;
    info._extent         = glm::uvec3(8, 8, 1);
    info._format         = vkm::VkmFormat::R8G8B8A8_UNORM;
    info._numMipLevels   = 1;
    info._numArrayLayers = 1;
    info._debugName      = "PoolEnumerationTexture";

    std::vector<vkm::VkmResourceHandle> created;
    for (int i = 0; i < 3; ++i)
    {
        vkm::VkmTexture* texture = driver->newTexture(info);
        REQUIRE(texture != nullptr);
        created.push_back(texture->getHandle());
    }

    std::vector<vkm::VkmResourceHandle> handles = pool->getAllResourceHandles(vkm::VkmResourceType::Texture);
    CHECK(handles.size() == before + 3);
    for (vkm::VkmResourceHandle handle : created)
    {
        CHECK(std::find(handles.begin(), handles.end(), handle) != handles.end());
        // Every reported handle must still resolve -- that is the whole contract.
        CHECK(pool->getResource<vkm::VkmTexture>(handle) != nullptr);
    }

    pool->releaseResource(created[1]);
    handles = pool->getAllResourceHandles(vkm::VkmResourceType::Texture);
    CHECK(handles.size() == before + 2);
    CHECK(std::find(handles.begin(), handles.end(), created[1]) == handles.end());

    pool->releaseResource(created[0]);
    pool->releaseResource(created[2]);
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

/*
* The dependency edges the inspector's Graph tab draws come from VkmRenderGraph::compile(), through
* VkmRenderSubGraph::getDependentSubGraphIds() and into the capture. A write followed by a read of
* the same buffer is one edge, and a pass that touches nothing shared is none.
*/
TEST_CASE("Render graph capture - dependency edges follow the declared accesses") {
    CaptureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver;

    vkm::VkmBufferInfo bufferInfo{};
    bufferInfo._flags = static_cast<vkm::VkmResourceCreateInfo>(
        static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderReadWrite) |
        static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst));
    bufferInfo._size = 256;
    bufferInfo._debugName = "CaptureDependencyBuffer";
    vkm::VkmBuffer* shared = driver->newBuffer(bufferInfo);
    REQUIRE(shared != nullptr);

    bufferInfo._debugName = "CaptureUnrelatedBuffer";
    vkm::VkmBuffer* unrelated = driver->newBuffer(bufferInfo);
    REQUIRE(unrelated != nullptr);

    vkm::VkmRenderGraphCapture capture;
    capture.arm();

    vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
    auto* producer = renderGraph.beginComputeSubGraph("Producer");
    producer->addReferencedResource(shared->getHandle(), vkm::VkmResourceAccess::ShaderStorageWrite);
    auto* bystander = renderGraph.beginComputeSubGraph("Bystander");
    bystander->addReferencedResource(unrelated->getHandle(), vkm::VkmResourceAccess::ShaderStorageWrite);
    auto* consumer = renderGraph.beginComputeSubGraph("Consumer");
    consumer->addReferencedResource(shared->getHandle(), vkm::VkmResourceAccess::ShaderStorageRead);

    renderGraph.compile();
    renderGraph.execute(vkm::VkmRenderGraphCommitOptions{ .capture = &capture });
    renderGraph.ensureCompleted();
    capture.finalize(driver);

    REQUIRE(capture.getPasses().size() == 3);
    const vkm::VkmCapturedPass& capturedProducer = capture.getPasses()[0];
    const vkm::VkmCapturedPass& capturedBystander = capture.getPasses()[1];
    const vkm::VkmCapturedPass& capturedConsumer = capture.getPasses()[2];

    CHECK(capturedProducer.dependencies.empty());
    CHECK(capturedBystander.dependencies.empty());
    REQUIRE(capturedConsumer.dependencies.size() == 1);
    CHECK(capturedConsumer.dependencies[0] == capturedProducer.subGraphId);

    // The declared access rides along with each referenced resource: it is what tells the Graph
    // tab a pass reads a render target rather than producing it.
    REQUIRE(capturedProducer.referencedResources.size() == 1);
    CHECK(capturedProducer.referencedResources[0].access == vkm::VkmResourceAccess::ShaderStorageWrite);
    REQUIRE(capturedConsumer.referencedResources.size() == 1);
    CHECK(capturedConsumer.referencedResources[0].access == vkm::VkmResourceAccess::ShaderStorageRead);

    driver->getRenderResourcePool()->releaseResource(shared->getHandle());
    driver->getRenderResourcePool()->releaseResource(unrelated->getHandle());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
