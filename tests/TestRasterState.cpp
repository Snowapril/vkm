#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestRasterStateShared.hpp"

#include <memory>

namespace
{
    struct VulkanRasterStateFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanRasterStateFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanRasterStateFixture()
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

TEST_CASE("Vulkan raster state - wireframe fill mode draws an outline, not a solid triangle")
{
    VulkanRasterStateFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runWireframeFillModeTest(f.driver.get());
}

#endif // VKM_USE_VULKAN_API
