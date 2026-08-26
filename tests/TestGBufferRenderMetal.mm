#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestGBufferRenderShared.hpp"

#import <Metal/MTLDevice.h>

#include <memory>

/*
* Metal-only, matching TestSceneModelRenderMetal: the real-pixel scene-render path is verified on
* the backend this machine can actually run end to end (the Vulkan offscreen scene fixture is
* documented as rendering black and crashing on lavapipe -- TODO.md).
*/

namespace
{
    struct MetalGBufferRenderFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalGBufferRenderFixture()
        {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device == nil)
            {
                initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported,
                                                "No Metal device available on this system."};
                return;
            }
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::make_unique<vkm::VkmDriverMetal>(device);
            initResult = driver->initialize(&opts);
        }
        ~MetalGBufferRenderFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

TEST_CASE("Metal G-buffer - a scene draw fills every channel") {
    MetalGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runGBufferRenderTest(fixture.driver.get());
}

TEST_CASE("Metal material textures - the base-colour texture reaches the G-buffer, not just the factor") {
    MetalGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runMaterialTextureTest(fixture.driver.get());
}

TEST_CASE("Metal texture streaming - a streamed-out material texture is a new resource that still samples") {
    MetalGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTextureStreamingSwapTest(fixture.driver.get());
}

TEST_CASE("Metal texture streaming - turning the switch off returns a coarsened texture to its whole chain")
{
    MetalGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTextureStreamingDisableRestoresChainTest(fixture.driver.get());
}

TEST_CASE("Metal texture feedback - the shader reports the level it wanted and the streamer acts on it") {
    MetalGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTextureFeedbackTest(fixture.driver.get());
}

TEST_CASE("Metal deferred lighting - the G-buffer is sampled through set 2 and shaded") {
    MetalGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runDeferredLightingTest(fixture.driver.get());
}

TEST_CASE("Metal tonemap - the filmic curve is normalized to its white point") {
    MetalGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTonemapTest(fixture.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
