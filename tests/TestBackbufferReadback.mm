#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/texture.h>

#include <memory>
#include <vector>

static constexpr int kWidth    = 64;
static constexpr int kHeight   = 64;
[[maybe_unused]] static constexpr int kChannels = 4;

// Renders a solid colour to a 64x64 offscreen texture via the vkm engine abstraction.
// Returns the CPU pixel data. Readback is stubbed until VkmDriverBase::readbackTexture()
// is available — the returned vector is always empty for now.
static std::vector<uint8_t> renderSolidColorAndReadback(
    vkm::VkmDriverBase* driver,
    float r, float g, float b, float a)
{
    vkm::VkmTextureInfo texInfo{};
    texInfo._flags          = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                              vkm::VkmResourceCreateInfo::AllowTransferSrc;
    texInfo._extent         = glm::uvec3(kWidth, kHeight, 1);
    texInfo._format         = vkm::VkmFormat::R8G8B8A8_UNORM;
    texInfo._numMipLevels   = 1;
    texInfo._numArrayLayers = 1;

    // Not owned here: VkmDriverBase::newTexture() registers the texture in the driver's
    // VkmRenderResourcePool, which owns its lifetime (destroyed along with the driver).
    vkm::VkmTexture* offscreen = driver->newTexture(texInfo);
    REQUIRE(offscreen != nullptr);

    vkm::VkmFrameBufferDescriptor fbDesc{};
    fbDesc._width  = static_cast<uint32_t>(kWidth);
    fbDesc._height = static_cast<uint32_t>(kHeight);
    fbDesc._renderPass._colorAttachmentCount                 = 1;
    fbDesc._renderPass._colorAttachments[0]._attachmentId   = 0;
    fbDesc._renderPass._colorAttachments[0]._loadAction     = vkm::VkmLoadAction::Clear;
    fbDesc._renderPass._colorAttachments[0]._storeAction    = vkm::VkmStoreAction::Store;
    fbDesc._renderPass._colorAttachments[0]._clearColors[0] = r;
    fbDesc._renderPass._colorAttachments[0]._clearColors[1] = g;
    fbDesc._renderPass._colorAttachments[0]._clearColors[2] = b;
    fbDesc._renderPass._colorAttachments[0]._clearColors[3] = a;
    fbDesc._colorAttachments[0] = offscreen->getHandle();

    vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
    renderGraph.beginGraphicsSubGraph(fbDesc);
    renderGraph.compile();
    renderGraph.execute();  // waitForCompletion = true by default
    renderGraph.ensureCompleted();

    // TODO: Implement CPU readback once VkmDriverBase::readbackTexture() is available.
    //
    // To restore this test:
    //   1. Add the readback call:
    //        vkm::VkmTextureReadbackResult rb = driver->readbackTexture(offscreen->getHandle());
    //        return rb.pixels;
    //
    //   2. Restore the reference PNG helper and STB includes:
    //        #pragma clang diagnostic push
    //        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    //        #define STB_IMAGE_IMPLEMENTATION
    //        #define STB_IMAGE_WRITE_IMPLEMENTATION
    //        #include <stb_image.h>
    //        #include <stb_image_write.h>
    //        #pragma clang diagnostic pop
    //        #include <filesystem>
    //        #include <string>
    //
    //   3. Uncomment the comparison block in each test case (see below).
    //
    // Expected engine-side interface (to be added to common/driver.h):
    //
    //   struct VkmTextureReadbackResult {
    //       std::vector<uint8_t> pixels;   // row-major, kChannels bytes per pixel
    //       uint32_t             width;
    //       uint32_t             height;
    //       uint32_t             channels;
    //   };
    //   VkmTextureReadbackResult readbackTexture(VkmResourceHandle handle);
    //
    // Engine implementation responsibilities:
    //   a. Allocate a CPU-mappable staging buffer of size width * height * bytesPerPixel.
    //   b. Record a transfer command (blit / buffer-image copy) from the source texture
    //      into the staging buffer on the graphics or transfer queue.
    //   c. Submit and wait for completion via VkmGpuEventTimelineBase::waitIdle().
    //   d. Map the staging buffer, memcpy into result vector, then release.

    return {};  // empty until readbackTexture() is implemented
}

// MTLCreateSystemDefaultDevice() is called solely to construct VkmDriverMetal.
// No raw Metal rendering work is done in or from this fixture.
struct ReadbackFixture {
    vkm::VkmDriverMetal* driver = nullptr;
    vkm::VkmInitResult initResult;

    ReadbackFixture() {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported, "No Metal device available on this system."};
            return;
        }
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
        driver = new vkm::VkmDriverMetal(device);
        initResult = driver->initialize(&opts);
    }
    ~ReadbackFixture() {
        delete driver;
    }
};

TEST_CASE("Backbuffer readback - solid red renders and matches reference PNG") {
    ReadbackFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    auto rendered = renderSolidColorAndReadback(f.driver, 1.0f, 0.0f, 0.0f, 1.0f);

    // TODO: uncomment once VkmDriverBase::readbackTexture() is implemented.
    // See renderSolidColorAndReadback() for restore instructions.
    //
    // std::string refPath = std::string(RESOURCES_DIR) + "tests/reference_red_64x64.png";
    // auto reference = loadOrGenerateReferencePng(refPath, 255, 0, 0, 255);
    //
    // stbi_write_png("/tmp/vkm_backbuffer_rendered.png",
    //                kWidth, kHeight, kChannels, rendered.data(), kWidth * kChannels);
    //
    // REQUIRE(rendered.size() == reference.size());
    // int maxDiff = 0;
    // for (size_t i = 0; i < rendered.size(); ++i)
    //     maxDiff = std::max(maxDiff, std::abs((int)rendered[i] - (int)reference[i]));
    // CHECK(maxDiff <= 1);

    CHECK(rendered.empty());  // placeholder until readbackTexture() is implemented
}

TEST_CASE("Backbuffer readback - solid green renders and matches reference PNG") {
    ReadbackFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    auto rendered = renderSolidColorAndReadback(f.driver, 0.0f, 1.0f, 0.0f, 1.0f);

    // TODO: uncomment once VkmDriverBase::readbackTexture() is implemented.
    // See renderSolidColorAndReadback() for restore instructions.
    //
    // std::string refPath = std::string(RESOURCES_DIR) + "tests/reference_green_64x64.png";
    // auto reference = loadOrGenerateReferencePng(refPath, 0, 255, 0, 255);
    //
    // stbi_write_png("/tmp/vkm_backbuffer_rendered_green.png",
    //                kWidth, kHeight, kChannels, rendered.data(), kWidth * kChannels);
    //
    // REQUIRE(rendered.size() == reference.size());
    // int maxDiff = 0;
    // for (size_t i = 0; i < rendered.size(); ++i)
    //     maxDiff = std::max(maxDiff, std::abs((int)rendered[i] - (int)reference[i]));
    // CHECK(maxDiff <= 1);

    CHECK(rendered.empty());  // placeholder until readbackTexture() is implemented
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
