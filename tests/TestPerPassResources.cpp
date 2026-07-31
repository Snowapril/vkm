#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestPerPassResourcesShared.hpp"

#include <memory>

namespace
{
    struct VulkanPerPassFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanPerPassFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanPerPassFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
            glfwTerminate();
        }
    };
}

TEST_CASE("Vulkan per-pass resources - a compute pass reads and writes only through set 2") {
    VulkanPerPassFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runPerPassResourceTest(fixture.driver.get());
}

TEST_CASE("Vulkan per-pass resources - a table is rejected when it does not match the declaration") {
    VulkanPerPassFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runPerPassValidationTest(fixture.driver.get());
}

#endif // VKM_USE_VULKAN_API
