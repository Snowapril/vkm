// Copyright (c) 2025 Snowapril

#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>

#ifdef VKM_USE_VULKAN_API

// Before GLFW: volk pulls in windows.h, which defines APIENTRY. GLFW only defines its own when
// none exists, so the reverse order is a macro redefinition -- a warning this build treats as an
// error.
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <GLFW/glfw3.h>

#include "TestUpscalerShared.hpp"

#include <memory>

// The Vulkan half of the temporal upscaler coverage. Self-skips wherever the capability is
// absent: builds without VKM_ENABLE_FSR (macOS/Linux), and Windows machines without the
// FidelityFX runtime DLL.

namespace
{
    struct VulkanUpscalerFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanUpscalerFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanUpscalerFixture()
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

// Above the default budget: FSR's context creation compiles its pipeline permutations on first
// use, a one-time cost this test pays on every run.
TEST_CASE("Vulkan temporal upscaler - upscales a jittered sequence to the display extent" *
          doctest::timeout(90.0))
{
    VulkanUpscalerFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTemporalUpscalerTest(fixture.driver.get());
    // Ratio 1.0, the mode the engine defaults to: FSR's own native-AA mode, where maxRenderSize
    // equals maxUpscaleSize.
    vkmtest::runTemporalUpscalerTest(fixture.driver.get(), glm::uvec2(640u, 360u),
                                     glm::uvec2(640u, 360u));
}

#endif // VKM_USE_VULKAN_API
