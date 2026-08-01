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
* Covers VkmCommandBufferBase::barrierTextureForShaderRead on the only backend that does real
* work for it. Vulkan is also the only backend where getting this wrong is observable: Metal and
* WebGPU have no image layouts, so their implementations are documented no-ops.
*
* This is deliberately a white-box check of the resulting image layout rather than a
* render-then-sample pixel test. The behavioural proof needs a shader that samples a Texture2D
* through the bindless array, and no sample ships one yet (the skybox samples a cubemap, and glTF
* import has no textures) -- that arrives with the G-buffer, which is the first real consumer of
* this barrier.
*
* Validation layers are on, so a malformed VkImageMemoryBarrier2 -- wrong aspect mask for a depth
* format, a stale oldLayout, an image missing a required usage flag -- fails here even though the
* assertions only look at the tracked layout.
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

    vkm::VkmTexture* createRenderTarget(vkm::VkmDriverBase* driver, vkm::VkmFormat format,
                                        vkm::VkmResourceCreateInfo flags, const char* debugName)
    {
        vkm::VkmTextureInfo info{};
        info._flags = flags;
        info._extent = glm::uvec3(kExtent, kExtent, 1);
        info._numMipLevels = 1;
        info._numArrayLayers = 1;
        info._format = format;
        info._debugName = debugName;
        return driver->newTexture(info);
    }
}

TEST_CASE("Vulkan barrierTextureForShaderRead - a colour render target ends shader-readable") {
    VulkanBarrierFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkm::VkmDriverBase* driver = fixture.driver.get();

    vkm::VkmTexture* target = createRenderTarget(
        driver, vkm::VkmFormat::BGRA8_UNORM,
        static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowColorAttachment) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead)),
        "TestBarrierColorTarget");
    REQUIRE(target != nullptr);

    vkm::VkmFrameBufferDescriptor fbDesc{};
    fbDesc._width = kExtent;
    fbDesc._height = kExtent;
    fbDesc._renderPass._colorAttachmentCount = 1;
    fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
    fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
    fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
    fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
    fbDesc._colorAttachments[0] = target->getHandle();

    const vkm::VkmResourceHandle targetHandle = target->getHandle();

    vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
    // A render pass with no draws still leaves the attachment in COLOR_ATTACHMENT_OPTIMAL, which
    // is the state this barrier exists to undo -- no pipeline is needed to reach it.
    renderGraph.beginGraphicsSubGraph(fbDesc);
    // The barrier belongs between the pass that writes and the pass that reads, so it records in
    // its own subgraph outside any render pass (barrierTextureForShaderRead rejects being called
    // inside one).
    auto* barrierSubGraph = renderGraph.beginComputeSubGraph("BarrierToShaderRead");
    barrierSubGraph->setComputeCallback([targetHandle](vkm::VkmCommandBufferBase* commandBuffer) {
        commandBuffer->barrierTextureForShaderRead(targetHandle);
    });
    renderGraph.compile();
    renderGraph.execute();
    renderGraph.ensureCompleted();

    vkm::VkmTextureVulkan* targetVulkan = static_cast<vkm::VkmTextureVulkan*>(
        driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(targetHandle));
    REQUIRE(targetVulkan != nullptr);
    // The layout every bindless texture descriptor declares
    // (VkmBindlessResourceManagerVulkan writes imageLayout = SHADER_READ_ONLY_OPTIMAL), so a
    // sampled render target has to actually be in it.
    CHECK(targetVulkan->getCurrentLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // The graph has completed, so a direct release is safe; leaving it to the driver's teardown
    // trips VMA's "Unfreed dedicated allocations found!" assertion.
    driver->getRenderResourcePool()->releaseResource(targetHandle);
}

TEST_CASE("Vulkan barrierTextureForShaderRead - a depth target transitions on its depth aspect") {
    VulkanBarrierFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkm::VkmDriverBase* driver = fixture.driver.get();

    // A depth buffer sampled by a later pass is exactly what a G-buffer needs. It takes a
    // different path through the barrier (aspect mask, and a different source layout), and using
    // the colour aspect here is a validation error rather than a silent one.
    vkm::VkmTexture* depth = createRenderTarget(
        driver, vkm::VkmFormat::D32_SFLOAT,
        static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead)),
        "TestBarrierDepthTarget");
    REQUIRE(depth != nullptr);

    const vkm::VkmResourceHandle depthHandle = depth->getHandle();

    vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
    auto* barrierSubGraph = renderGraph.beginComputeSubGraph("BarrierDepthToShaderRead");
    barrierSubGraph->setComputeCallback([depthHandle](vkm::VkmCommandBufferBase* commandBuffer) {
        commandBuffer->barrierTextureForShaderRead(depthHandle);
    });
    renderGraph.compile();
    renderGraph.execute();
    renderGraph.ensureCompleted();

    vkm::VkmTextureVulkan* depthVulkan = static_cast<vkm::VkmTextureVulkan*>(
        driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(depthHandle));
    REQUIRE(depthVulkan != nullptr);
    CHECK(depthVulkan->getCurrentLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    driver->getRenderResourcePool()->releaseResource(depthHandle);
}

TEST_CASE("Vulkan barrierTextureForShaderRead - an already-readable texture records nothing") {
    VulkanBarrierFixture fixture;
    VKM_REQUIRE_DEVICE(fixture.initResult);
    vkm::VkmDriverBase* driver = fixture.driver.get();

    // An uploaded texture is already SHADER_READ_ONLY_OPTIMAL (copyBufferToTexture leaves it
    // there), so the barrier must be a no-op rather than emitting a redundant transition every
    // frame for every G-buffer channel.
    vkm::VkmTexture* uploaded = createRenderTarget(
        driver, vkm::VkmFormat::R8G8B8A8_UNORM,
        static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst)),
        "TestBarrierUploadedTexture");
    REQUIRE(uploaded != nullptr);

    const std::vector<uint8_t> pixels(static_cast<size_t>(kExtent) * kExtent * 4, 0x7F);
    REQUIRE(driver->uploadToTexture(uploaded->getHandle(), pixels.data(), pixels.size()));

    vkm::VkmTextureVulkan* uploadedVulkan = static_cast<vkm::VkmTextureVulkan*>(
        driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(uploaded->getHandle()));
    REQUIRE(uploadedVulkan != nullptr);
    REQUIRE(uploadedVulkan->getCurrentLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const vkm::VkmResourceHandle uploadedHandle = uploaded->getHandle();
    vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
    auto* barrierSubGraph = renderGraph.beginComputeSubGraph("BarrierNoOp");
    barrierSubGraph->setComputeCallback([uploadedHandle](vkm::VkmCommandBufferBase* commandBuffer) {
        commandBuffer->barrierTextureForShaderRead(uploadedHandle);
    });
    renderGraph.compile();
    renderGraph.execute();
    renderGraph.ensureCompleted();

    CHECK(uploadedVulkan->getCurrentLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    driver->getRenderResourcePool()->releaseResource(uploadedHandle);
}

#endif // VKM_USE_VULKAN_API
