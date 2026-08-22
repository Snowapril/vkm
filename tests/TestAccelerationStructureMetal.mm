// Copyright (c) 2026 Snowapril

#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>

#ifdef VKM_USE_METAL_API

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestAccelerationStructureShared.hpp"
#include "TestSceneAccelerationStructureShared.hpp"
#include "TestRayQueryShared.hpp"
#include "TestPathTracerShared.hpp"
#include "TestIndirectPassShared.hpp"
#include "TestNeeShared.hpp"

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

TEST_CASE("Path tracer - white furnace and energy conservation on Metal" * doctest::timeout(120.0))
{
    MetalAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runPathTracerFurnaceTest(fixture.driver.get());
}

TEST_CASE("Indirect pass - 1 spp converges to the reference path tracer on Metal" * doctest::timeout(400.0))
{
    MetalAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runIndirectConvergenceTest(fixture.driver.get());
}

TEST_CASE("NEE - the area estimator matches the analytic plane on Metal" * doctest::timeout(400.0))
{
    MetalAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runNeeAnalyticPlaneTest(fixture.driver.get());
}

TEST_CASE("NEE - the emissive Cornell converges deferred against reference on Metal" * doctest::timeout(400.0))
{
    MetalAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    // Measured floor 8.8e-4 at 1536 samples: the deferred side finds the small ceiling
    // patch by BSDF sampling at its first vertex (high variance) while the reference uses
    // NEE (low), so this comparison's floor sits far above the environment-lit Cornell's.
    // The mean-ratio check inside is the bias detector; this bound catches gross breakage.
    vkmtest::runNeeEmissiveCornellTest(fixture.driver.get(), /*mseThreshold=*/1.5e-3f);
}

#endif // VKM_USE_METAL_API
