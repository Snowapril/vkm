#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestPerPassResourcesShared.hpp"

#import <Metal/MTLDevice.h>

#include <memory>

namespace
{
    struct MetalPerPassFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalPerPassFixture()
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
        ~MetalPerPassFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

TEST_CASE("Metal per-pass resources - a compute pass reads and writes only through set 2") {
    MetalPerPassFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runPerPassResourceTest(fixture.driver.get());
}

TEST_CASE("Metal per-pass resources - a table is rejected when it does not match the declaration") {
    MetalPerPassFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runPerPassValidationTest(fixture.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
