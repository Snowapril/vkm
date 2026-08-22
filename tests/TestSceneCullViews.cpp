#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

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

// The plane convention vkmBuildBoxPlanes shares with vkmExtractFrustumPlanes and scene_cull.hlsl:
// inward-facing, so dot(normal, centre) + w is the signed distance and a value below -radius
// rejects the sphere. Nothing asserted this before the shadow atlas gained a second caller, and a
// flipped sign is the kind of mistake that culls everything or nothing rather than erroring.
TEST_CASE("vkmBuildBoxPlanes - inward-facing planes, positive inside the box") {
    glm::vec4 planes[6];
    vkm::vkmBuildBoxPlanes(glm::vec3(-1.0f, -2.0f, -3.0f), glm::vec3(4.0f, 5.0f, 6.0f), planes);

    const auto signedDistance = [&](uint32_t plane, const glm::vec3& point) {
        return glm::dot(glm::vec3(planes[plane]), point) + planes[plane].w;
    };

    const glm::vec3 inside(1.0f, 1.0f, 1.0f);
    for (uint32_t plane = 0; plane < 6; ++plane)
    {
        CHECK(signedDistance(plane, inside) > 0.0f);
    }

    // Just outside the -X face: exactly one plane rejects it, and by the distance it overshot.
    CHECK(signedDistance(0, glm::vec3(-1.5f, 1.0f, 1.0f)) == doctest::Approx(-0.5f));
    // And just outside +Z.
    CHECK(signedDistance(5, glm::vec3(1.0f, 1.0f, 6.25f)) == doctest::Approx(-0.25f));

    // On a face the distance is exactly zero, which is what makes the cull's `< -radius` test
    // keep an object touching the boundary.
    CHECK(signedDistance(0, glm::vec3(-1.0f, 1.0f, 1.0f)) == doctest::Approx(0.0f));
}
