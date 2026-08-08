#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

#include "TestAliasedTextureShared.hpp"

#include <memory>

namespace
{
    struct VulkanAliasedTextureFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanAliasedTextureFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanAliasedTextureFixture()
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

TEST_CASE("Vulkan aliasable texture - the flag survives only where it can be served")
{
    VulkanAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasableFlagSanitizerTest(f.driver.get());
}

TEST_CASE("Vulkan aliasable buffer - the flag is texture-only and dropped")
{
    VulkanAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasableBufferFlagIsDroppedTest(f.driver.get());
}

TEST_CASE("Vulkan aliased textures - disjoint lifetimes share bytes")
{
    VulkanAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasedPlacementTest(f.driver.get());
}

TEST_CASE("Vulkan aliased textures - overlapping lifetimes keep their own bytes")
{
    VulkanAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runOverlappingLifetimeIsRefusedTest(f.driver.get());
}

TEST_CASE("Vulkan aliased textures - an undeclared attachment widens the lifetime")
{
    VulkanAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runUndeclaredAliasedUseTest(f.driver.get());
}

TEST_CASE("Vulkan aliased textures - each reads back its own contents" * doctest::timeout(10.0))
{
    VulkanAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasedPixelTest(f.driver.get());
}

#endif // VKM_USE_VULKAN_API
