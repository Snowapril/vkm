// Copyright (c) 2026 Snowapril
//
// The Vulkan half of the scene-render coverage that was Metal-only until now.
//
// It exists because the deferred GI test -- the first thing that ever ran the engine's G-buffer
// raster path on Vulkan -- reported zero covered pixels on lavapipe while the reference path
// tracer covered all of them. That says the rasteriser drew nothing, and nothing in the Vulkan
// test suite could say so, because the two tests that draw a scene were both Metal-only.
//
// Running the same shared bodies here reproduces that without a ray-tracing device, so MoltenVK
// (which reports no ray tracing and skips every test above) is enough to see it.

#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestGBufferRenderShared.hpp"

#include <memory>

namespace
{
    struct VulkanGBufferRenderFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanGBufferRenderFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanGBufferRenderFixture()
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

TEST_CASE("Vulkan G-buffer - a scene draw fills every channel")
{
    VulkanGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runGBufferRenderTest(fixture.driver.get());
}

TEST_CASE("Vulkan material textures - the base-colour texture reaches the G-buffer, not just the factor")
{
    VulkanGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runMaterialTextureTest(fixture.driver.get());
}

TEST_CASE("Vulkan texture streaming - a streamed-out material texture is a new resource that still samples")
{
    VulkanGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTextureStreamingSwapTest(fixture.driver.get());
}

TEST_CASE("Vulkan texture feedback - the shader reports the level it wanted and the streamer acts on it")
{
    VulkanGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runTextureFeedbackTest(fixture.driver.get());
}

TEST_CASE("Vulkan deferred lighting - the G-buffer is sampled through set 2 and shaded")
{
    VulkanGBufferRenderFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkmtest::runDeferredLightingTest(fixture.driver.get());
}

#endif // VKM_USE_VULKAN_API
