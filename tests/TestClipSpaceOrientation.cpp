#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestClipSpaceOrientationShared.hpp"

#include <memory>

namespace
{
    struct VulkanClipSpaceFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanClipSpaceFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanClipSpaceFixture()
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

TEST_CASE("Vulkan clip space - +Y-up triangle renders right way up")
{
    VulkanClipSpaceFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runClipSpaceOrientationTest(f.driver.get());
}

#endif // VKM_USE_VULKAN_API
