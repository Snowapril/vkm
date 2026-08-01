#ifndef TEST_VIEWPORT_SHARED_HPP
#define TEST_VIEWPORT_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>

#include <array>
#include <string>
#include <vector>

/*
* Covers setViewportAndScissor by drawing a fullscreen triangle into part of an attachment.
*
* The capability being tested is what makes a probe update affordable: without it, a probe's six
* cube faces need six render passes, because they cannot be packed into one attachment. So the
* assertions are about texels *outside* the viewport being untouched, not just about something
* having been drawn -- a viewport that is silently ignored still fills the target, and looks fine.
*/

namespace vkmtest
{
    inline void runViewportTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kSize = 64;
        constexpr uint32_t kHalf = kSize / 2;

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_VIEWPORT_PSO_DIR, TEST_VIEWPORT_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::User, &psoError),
                        psoError);
        vkm::VkmPipelineStateBase* pso =
            manager.getPipelineState("viewport_fill_pso", vkm::VkmPipelineStateOrigin::User);
        REQUIRE(pso != nullptr);

        vkm::VkmTextureInfo targetInfo{};
        targetInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc;
        targetInfo._extent = glm::uvec3(kSize, kSize, 1);
        targetInfo._numMipLevels = 1;
        targetInfo._numArrayLayers = 1;
        targetInfo._format = vkm::VkmFormat::R8G8B8A8_UNORM;
        targetInfo._debugName = "ViewportTarget";
        vkm::VkmTexture* target = driver->newTexture(targetInfo);
        REQUIRE(target != nullptr);

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = kSize;
        fbDesc._height = kSize;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f; // opaque black
        fbDesc._colorAttachments[0] = target->getHandle();

        // Draws the fullscreen triangle once per rectangle, each restricted to that rectangle.
        // Several draws into one attachment is exactly the probe-update usage.
        const auto renderInto = [&](const std::vector<std::array<uint32_t, 4>>& rects) {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
            subGraph->setRenderCallback([pso, &rects](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pso);
                for (const std::array<uint32_t, 4>& rect : rects)
                {
                    commandBuffer->setViewportAndScissor(static_cast<int32_t>(rect[0]),
                                                         static_cast<int32_t>(rect[1]), rect[2], rect[3]);
                    commandBuffer->draw(3, 1, 0, 0);
                }
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
            return driver->readbackTexture(target->getHandle());
        };

        const auto isWhite = [](const vkm::VkmTextureReadbackResult& readback, uint32_t x, uint32_t y) {
            const uint8_t* texel = &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
            return texel[0] > 200 && texel[1] > 200 && texel[2] > 200;
        };

        SUBCASE("a viewport confines the draw to its rectangle")
        {
            // Top-left quadrant only.
            const vkm::VkmTextureReadbackResult readback = renderInto({{{0u, 0u, kHalf, kHalf}}});
            REQUIRE(readback.width == kSize);

            CHECK(isWhite(readback, kHalf / 2, kHalf / 2));           // inside
            // The three quadrants the viewport excludes must still hold the clear colour. A
            // backend that ignored the call would fill all of them and still look plausible.
            CHECK_FALSE(isWhite(readback, kSize - kHalf / 2, kHalf / 2));
            CHECK_FALSE(isWhite(readback, kHalf / 2, kSize - kHalf / 2));
            CHECK_FALSE(isWhite(readback, kSize - kHalf / 2, kSize - kHalf / 2));
        }

        SUBCASE("the origin is the attachment's top-left on every backend")
        {
            // A rectangle only in the lower-right quadrant. If a backend measured y from the
            // bottom, the white would land in the upper-right instead -- which is the
            // cross-backend convention this pins.
            const vkm::VkmTextureReadbackResult readback = renderInto({{{kHalf, kHalf, kHalf, kHalf}}});
            CHECK(isWhite(readback, kSize - kHalf / 2, kSize - kHalf / 2)); // lower-right
            CHECK_FALSE(isWhite(readback, kSize - kHalf / 2, kHalf / 2));   // upper-right
            CHECK_FALSE(isWhite(readback, kHalf / 2, kHalf / 2));           // upper-left
        }

        SUBCASE("several viewports in one pass each write their own tile")
        {
            // The probe-update pattern: pack multiple views into one attachment in a single pass.
            const vkm::VkmTextureReadbackResult readback =
                renderInto({{{0u, 0u, kHalf, kHalf}}, {{kHalf, kHalf, kHalf, kHalf}}});
            CHECK(isWhite(readback, kHalf / 2, kHalf / 2));                 // first tile
            CHECK(isWhite(readback, kSize - kHalf / 2, kSize - kHalf / 2)); // second tile
            // The untouched diagonal proves the second draw did not simply overwrite everything.
            CHECK_FALSE(isWhite(readback, kSize - kHalf / 2, kHalf / 2));
            CHECK_FALSE(isWhite(readback, kHalf / 2, kSize - kHalf / 2));
        }

        driver->getRenderResourcePool()->releaseResource(target->getHandle());
    }
}

#endif // TEST_VIEWPORT_SHARED_HPP
