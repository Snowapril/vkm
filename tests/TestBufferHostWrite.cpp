#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestBufferHostWriteShared.hpp"

#include <memory>

namespace
{
    struct VulkanBufferHostWriteFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanBufferHostWriteFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanBufferHostWriteFixture()
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

TEST_CASE("Vulkan buffer - the host-write and staging upload paths agree")
{
    VulkanBufferHostWriteFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runBufferHostWriteTest(f.driver.get());
}

TEST_CASE("Vulkan buffer - GPU addresses follow the device capability")
{
    VulkanBufferHostWriteFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runBufferGpuAddressTest(f.driver.get());
}

#endif // VKM_USE_VULKAN_API
