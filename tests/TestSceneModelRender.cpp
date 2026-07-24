#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestSceneModelRenderShared.hpp"

#include <memory>

namespace
{
    struct VulkanSceneModelFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanSceneModelFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanSceneModelFixture()
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

TEST_CASE("Vulkan scene model - an imported glTF mesh renders through the bindless path")
{
    VulkanSceneModelFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runSceneModelRenderTest(f.driver.get());
}

#endif // VKM_USE_VULKAN_API
