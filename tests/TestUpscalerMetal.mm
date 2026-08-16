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
          doctest::timeout(60.0))
{
    MetalUpscalerFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTemporalUpscalerTest(fixture.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
