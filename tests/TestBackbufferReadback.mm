#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/texture.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

static constexpr int kWidth    = 64;
static constexpr int kHeight   = 64;
[[maybe_unused]] static constexpr int kChannels = 4;

// Renders a solid colour to a 64x64 offscreen texture via the vkm engine abstraction and
// reads the pixels back through VkmDriverBase::readbackTexture().
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

    vkm::VkmTextureReadbackResult readback = driver->readbackTexture(offscreen->getHandle());
    return readback.pixels;
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

// Checks every pixel of an R8G8B8A8 image equals the expected RGBA value (within one
// LSB of quantization tolerance). Direct pixel asserts instead of the PNG-reference
// comparison originally sketched here -- see implementation-notes.md.
static void checkSolidColor(const std::vector<uint8_t>& pixels, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    REQUIRE(pixels.size() == static_cast<size_t>(kWidth) * kHeight * kChannels);
    int maxDiff = 0;
    const uint8_t expected[4] = {r, g, b, a};
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        maxDiff = std::max(maxDiff, std::abs(static_cast<int>(pixels[i]) - static_cast<int>(expected[i % 4])));
    }
    CHECK(maxDiff <= 1);
}

TEST_CASE("Backbuffer readback - solid red renders and reads back") {
    ReadbackFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    auto rendered = renderSolidColorAndReadback(f.driver, 1.0f, 0.0f, 0.0f, 1.0f);
    checkSolidColor(rendered, 255, 0, 0, 255);
}

TEST_CASE("Backbuffer readback - solid green renders and reads back") {
    ReadbackFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    auto rendered = renderSolidColorAndReadback(f.driver, 0.0f, 1.0f, 0.0f, 1.0f);
    checkSolidColor(rendered, 0, 255, 0, 255);
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
