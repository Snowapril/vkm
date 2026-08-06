// Copyright (c) 2025 Snowapril

#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>

#ifdef VKM_USE_METAL_API

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestAccelerationStructureShared.hpp"
#include "TestSceneAccelerationStructureShared.hpp"
#include "TestRayQueryShared.hpp"

#include <memory>

namespace
{
    struct MetalAccelerationStructureFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalAccelerationStructureFixture()
        {
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverMetal>(new vkm::VkmDriverMetal(MTLCreateSystemDefaultDevice()));
            initResult = driver->initialize(&opts);
        }
        ~MetalAccelerationStructureFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
} // namespace

TEST_CASE("VkmAccelerationStructure - build, instance and rebuild on Metal")
{
    MetalAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runAccelerationStructureTest(fixture.driver.get());
}

TEST_CASE("VkmScene - acceleration structures out of the geometry pool on Metal")
{
    MetalAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runSceneAccelerationStructureTest(fixture.driver.get());
}

TEST_CASE("Ray query - a compute shader traces a loaded scene on Metal")
{
    MetalAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runRayQueryTest(fixture.driver.get());
}

#endif // VKM_USE_METAL_API
