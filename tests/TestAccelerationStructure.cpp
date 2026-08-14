// Copyright (c) 2025 Snowapril

#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestAccelerationStructureShared.hpp"
#include "TestSceneAccelerationStructureShared.hpp"
#include "TestRayQueryShared.hpp"
#include "TestPathTracerShared.hpp"
#include "TestIndirectPassShared.hpp"
#include "TestNeeShared.hpp"

#include <memory>

namespace
{
    struct VulkanAccelerationStructureFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanAccelerationStructureFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanAccelerationStructureFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
            glfwTerminate();
        }
    };
} // namespace

// On this machine MoltenVK reports no ray tracing and the shared body skips; the first backend
// that actually runs it is CI's lavapipe job, which does report the capability.
TEST_CASE("VkmAccelerationStructure - build, instance and rebuild on Vulkan")
{
    VulkanAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runAccelerationStructureTest(fixture.driver.get());
}

TEST_CASE("VkmScene - acceleration structures out of the geometry pool on Vulkan")
{
    VulkanAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runSceneAccelerationStructureTest(fixture.driver.get());
}

TEST_CASE("Ray query - a compute shader traces a loaded scene on Vulkan")
{
    VulkanAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runRayQueryTest(fixture.driver.get());
}

TEST_CASE("Path tracer - white furnace and energy conservation on Vulkan" * doctest::timeout(120.0))
{
    VulkanAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runPathTracerFurnaceTest(fixture.driver.get());
}

TEST_CASE("Indirect pass - 1 spp converges to the reference path tracer on Vulkan" * doctest::timeout(400.0))
{
    VulkanAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    // 1.0e-3 rather than the 6.0e-4 Metal runs at. lavapipe's floor here is 7.9e-4 against
    // Metal's 2.4e-4, and the gap is systematic -- eight times the samples moves it by 14% --
    // so no shared threshold separates a one-bounce error from the noise on both. This one
    // still does on lavapipe: that sabotage adds ~4.9e-4, which reads about 1.28e-3 there.
    vkmtest::runIndirectConvergenceTest(fixture.driver.get(), /*mseThreshold=*/1.0e-3f);
}

TEST_CASE("NEE - the area estimator matches the analytic plane on Vulkan" * doctest::timeout(400.0))
{
    VulkanAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runNeeAnalyticPlaneTest(fixture.driver.get());
}

TEST_CASE("NEE - the emissive Cornell converges deferred against reference on Vulkan" * doctest::timeout(400.0))
{
    VulkanAccelerationStructureFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    // Same shape as Metal's bound (measured floor 8.8e-4 there, see the .mm), plus the
    // lavapipe systematic-difference headroom the environment-lit Cornell gate carries.
    vkmtest::runNeeEmissiveCornellTest(fixture.driver.get(), /*mseThreshold=*/2.5e-3f);
}

#endif // VKM_USE_VULKAN_API
