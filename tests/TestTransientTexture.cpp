#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestTransientTextureShared.hpp"

#include <memory>

namespace
{
    struct VulkanTransientTextureFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanTransientTextureFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanTransientTextureFixture()
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

TEST_CASE("Vulkan transient texture - lands in the Transient pool and reports what it got")
{
    VulkanTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientTextureCreationTest(f.driver.get());
}

TEST_CASE("Vulkan transient texture - an unbackable request is downgraded, not rejected")
{
    VulkanTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientTextureIncompatibleFlagsTest(f.driver.get());
}

TEST_CASE("Vulkan transient texture - a heap placement hint does not cost it its tile memory")
{
    VulkanTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientHeapHintTest(f.driver.get());
}

TEST_CASE("Vulkan transient buffer - the flag is texture-only and dropped")
{
    VulkanTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientBufferFlagIsDroppedTest(f.driver.get());
}

TEST_CASE("Vulkan transient texture - a render pass with a transient depth attachment")
{
    VulkanTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientDepthRenderPassTest(f.driver.get(), /*storeDepth=*/false);
}

TEST_CASE("Vulkan transient texture - storing a transient attachment is coerced away")
{
    VulkanTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientDepthRenderPassTest(f.driver.get(), /*storeDepth=*/true);
}

#endif // VKM_USE_VULKAN_API
