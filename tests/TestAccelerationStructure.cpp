// Copyright (c) 2025 Snowapril

#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestAccelerationStructureShared.hpp"
#include "TestSceneAccelerationStructureShared.hpp"

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

#endif // VKM_USE_VULKAN_API
