#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestUpscalerShared.hpp"

#import <Metal/MTLDevice.h>

#include <memory>

namespace
{
    struct MetalUpscalerFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalUpscalerFixture()
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
        ~MetalUpscalerFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

// Above the default budget: requiresSynchronousInitialization compiles the scaler's pipelines
// inside newUpscaler, a one-time cost this test pays on every run.
TEST_CASE("Metal temporal upscaler - upscales a jittered sequence to the display extent" *
          doctest::timeout(90.0))
{
    MetalUpscalerFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTemporalUpscalerTest(fixture.driver.get());
    // Ratio 1.0, the mode the engine defaults to. MetalFX reports a minimum input content scale
    // of 1.0, so this is the bottom of its supported range rather than a special case. Only
    // reached when UnitTests runs without MTL_DEBUG_LAYER, which the test scripts always inject.
    vkmtest::runTemporalUpscalerTest(fixture.driver.get(), glm::uvec2(640u, 360u),
                                     glm::uvec2(640u, 360u));
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
