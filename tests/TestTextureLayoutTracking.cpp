#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>

#include <memory>

/*
* Covers VkmTextureVulkan's per-subresource image layout tracking on the only backend that has
* work for it. Vulkan is also the only backend where getting this wrong is observable: Metal and
* WebGPU have no image layouts, so their implementations are documented no-ops.
*
* A white-box check of the tracked layout rather than a render-then-sample pixel test: what a
* wrong answer here produces is a stale oldLayout, and validation layers are on, so a malformed
* VkImageMemoryBarrier2 fails the run even though the assertions only read the tracker.
*
* Per tests/CLAUDE.md, all GPU work here is driven through VkmRenderGraph; the only backend-specific
* use is reading VkmTextureVulkan's tracked layout to assert on, which is the "validity assertion"
* case that document permits, not driving work through a raw handle.
*/

namespace
{
    struct VulkanBarrierFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;

        VulkanBarrierFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanBarrierFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
            glfwTerminate();
        }
    };

    constexpr uint32_t kExtent = 32;
}

/*
* Layout is tracked per (mip, layer), and a cubemap upload is the case that proves it has to be:
* the six faces arrive as six separate copies, so after the first one exactly one layer is
* TRANSFER_DST/SHADER_READ and the other five are still UNDEFINED. A whole-image transition per
* face would record all six as written when only one had been -- and then name a stale oldLayout
* on the next face, which the validation layer running over this test would reject.
*/
TEST_CASE("Vulkan texture layout - a cubemap upload transitions one face at a time") {
    VulkanBarrierFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkm::VkmDriverBase* driver = fixture.driver.get();

    vkm::VkmTextureInfo info{};
    info._flags = static_cast<vkm::VkmResourceCreateInfo>(
        static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
        static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst));
    info._extent = glm::uvec3(kExtent, kExtent, 1);
    info._numMipLevels = 1;
    info._numArrayLayers = vkm::kVkmCubeFaceCount;
    info._format = vkm::VkmFormat::R8G8B8A8_UNORM;
    info._type = vkm::VkmTextureType::Cube;
    info._debugName = "TestSubresourceLayoutCubemap";

    vkm::VkmTexture* cubemap = driver->newTexture(info);
    REQUIRE(cubemap != nullptr);
    const vkm::VkmResourceHandle handle = cubemap->getHandle();

    vkm::VkmTextureVulkan* cubemapVulkan = static_cast<vkm::VkmTextureVulkan*>(
        driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(handle));
    REQUIRE(cubemapVulkan != nullptr);

    // Nothing written yet, so every face shares UNDEFINED and the tracker is still in its
    // one-VkImageLayout form -- the shape the common case has to keep.
    CHECK(cubemapVulkan->isLayoutUniform());
    CHECK(cubemapVulkan->getSubresourceLayout(0, 0) == VK_IMAGE_LAYOUT_UNDEFINED);

    const std::vector<uint8_t> face(static_cast<size_t>(kExtent) * kExtent * 4, 0x40);
    REQUIRE(driver->uploadToTexture(handle, face.data(), face.size(), /*mipLevel=*/0, /*arrayLayer=*/0));

    // One face in: that face is readable and the other five are untouched, which a whole-image
    // transition could not express.
    CHECK(cubemapVulkan->getSubresourceLayout(0, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(cubemapVulkan->getSubresourceLayout(0, 1) == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK_FALSE(cubemapVulkan->isLayoutUniform());
    CHECK(cubemapVulkan->getUniformLayout(vkm::VkmSubresourceRange{}) == VK_IMAGE_LAYOUT_MAX_ENUM);

    for (uint32_t layer = 1; layer < vkm::kVkmCubeFaceCount; ++layer)
    {
        REQUIRE(driver->uploadToTexture(handle, face.data(), face.size(), /*mipLevel=*/0, layer));
    }

    // All six agree again, so the tracker collapses back and stops paying for the vector. Without
    // this the texture would stay in the per-subresource form for the rest of its life.
    CHECK(cubemapVulkan->isLayoutUniform());
    for (uint32_t layer = 0; layer < vkm::kVkmCubeFaceCount; ++layer)
    {
        CHECK(cubemapVulkan->getSubresourceLayout(0, layer) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    driver->getRenderResourcePool()->releaseResource(handle);
}

#endif // VKM_USE_VULKAN_API
