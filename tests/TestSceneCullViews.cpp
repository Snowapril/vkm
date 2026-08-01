#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

// Gated on the engine PSO directory rather than on a backend: the cull and emit pipelines are
// engine PSOs, and that macro is defined exactly where a shader cache for them exists.
#if defined(TEST_ENGINE_PIPELINE_DIR)

#include "TestSceneCullViewsShared.hpp"

#include <memory>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

namespace
{
    struct VulkanCullViewsFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanCullViewsFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanCullViewsFixture()
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

TEST_CASE("Vulkan scene cull - two views in one frame keep their own frusta and results") {
    VulkanCullViewsFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runSceneCullViewsTest(fixture.driver.get());
}

#endif // VKM_USE_VULKAN_API

#endif // TEST_ENGINE_PIPELINE_DIR
