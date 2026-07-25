#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestCubemapTextureShared.hpp"

#include <memory>

namespace
{
    struct VulkanCubemapFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanCubemapFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanCubemapFixture()
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

TEST_CASE("Vulkan cubemap - six faces upload, read back, and register bindless")
{
    VulkanCubemapFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runCubemapTextureTest(f.driver.get());
}

TEST_CASE("Vulkan skybox - cubemap faces land in the right screen regions")
{
    VulkanCubemapFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runSkyboxRenderTest(f.driver.get());
}

#endif // VKM_USE_VULKAN_API
