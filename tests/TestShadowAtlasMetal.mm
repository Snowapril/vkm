#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestShadowAtlasShared.hpp"

#import <Metal/MTLDevice.h>

#include <memory>

namespace
{
    struct MetalShadowAtlasFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalShadowAtlasFixture()
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
        ~MetalShadowAtlasFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

TEST_CASE("Metal shadow atlas - a tile holds the true distance from its light") {
    MetalShadowAtlasFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runShadowAtlasTest(fixture.driver.get());
}

TEST_CASE("Metal shadow atlas - tile accounting per light type") {
    MetalShadowAtlasFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runShadowAtlasAllocationTest(fixture.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
