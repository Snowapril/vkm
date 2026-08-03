#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestResourceTablesShared.hpp"

#import <Metal/MTLDevice.h>

#include <memory>

namespace
{
    struct MetalResourceTableFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalResourceTableFixture()
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
        ~MetalResourceTableFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

TEST_CASE("Metal resource tables - sets 2 and 3 both reach a compute pass") {
    MetalResourceTableFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runResourceTableTest(fixture.driver.get());
}

TEST_CASE("Metal resource tables - set 3 lands at set 3 when set 2 is not declared") {
    MetalResourceTableFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runPerDrawOnlyTableTest(fixture.driver.get());
}

TEST_CASE("Metal resource tables - a table is rejected when it does not match the declaration") {
    MetalResourceTableFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runResourceTableValidationTest(fixture.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
