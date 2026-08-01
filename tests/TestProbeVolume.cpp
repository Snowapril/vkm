#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

// Backend-agnostic, and one of its tests needs no driver at all, so it is included unconditionally.
#include "TestProbeVolumeUpdaterShared.hpp"

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestProbeVolumeShared.hpp"

#include <memory>

namespace
{
    struct VulkanProbeVolumeFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanProbeVolumeFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanProbeVolumeFixture()
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

TEST_CASE("Vulkan probe volume - atlas addressing and history") {
    VulkanProbeVolumeFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runProbeVolumeTest(fixture.driver.get());
}

TEST_CASE("Vulkan probe update - the round-robin budget refreshes every probe once per round") {
    VulkanProbeVolumeFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runProbeUpdateScheduleTest(fixture.driver.get());
}

#endif // VKM_USE_VULKAN_API

// No driver and no backend: the convergence model is arithmetic over a budget and a hysteresis,
// and it is what the measured propagation latency is checked against.
TEST_CASE("Probe update - frames to converge follows the budget and the hysteresis") {
    vkmtest::runProbeConvergenceModelTest();
}
