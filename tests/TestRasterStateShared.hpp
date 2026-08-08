#ifndef TEST_RASTER_STATE_SHARED_HPP
#define TEST_RASTER_STATE_SHARED_HPP

// Backend-agnostic body of the rasterization-state regression test. Only the driver fixture is
// backend-specific (Metal needs MTLCreateSystemDefaultDevice() from Objective-C++), so the
// render + assertion logic lives here and is called from both TestRasterState.cpp (Vulkan) and
// TestRasterStateMetal.mm (Metal).
//
// What this proves: VkmRasterizationStateDescriptor::fillMode actually reaches the GPU. Vulkan and
// WebGPU bake it into the pipeline object, but on Metal fill mode is encoder state rather than part
// of MTLRenderPipelineState, so it has to be applied at bind time or the wireframe PSO silently
// renders solid.
//
// The test renders the triangle sample's "wireframe" variant and asserts the triangle's
// interior is empty. A solid fill covers that point; an outline does not.

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>

#include <array>
#include <cstdint>
#include <string>

namespace vkmtest
{
    // Matches triangle.hlsl's VertexData struct (see src/samples/triangle/main.cpp for the
    // padding rationale).
    struct RasterStateTriangleVertex
    {
        float position[3];
        float _pad0;
        float color[4];
    };

    // Renders the triangle sample's wireframe PSO variant into a 64x64 offscreen BGRA8 target
    // and asserts only the outline is drawn. All GPU work goes through engine abstractions only
    // (see tests/CLAUDE.md).
    inline void runWireframeFillModeTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kSize = 64;

        vkm::VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        REQUIRE(bindlessManager != nullptr);

        vkm::VkmPipelineStateManager manager(driver);
        std::string err;
        REQUIRE(manager.loadPipelineStatesFromDirectory(TEST_TRIANGLE_SAMPLE_DIR, TEST_TRIANGLE_SHADER_CACHE_DIR,
                                                        vkm::VkmPipelineStateOrigin::User, &err));
        // The wireframe variant declares a "backends" allowlist of vulkan+metal, so it exists on
        // both fixtures this header serves.
        vkm::VkmPipelineStateBase* pso =
            manager.getPipelineState("triangle_pso[wireframe]", vkm::VkmPipelineStateOrigin::User);
        REQUIRE(pso != nullptr);

        // Same clip-space triangle as the sample and the clip-space orientation test. The PSO
        // declares cull_mode "none", so winding does not matter here -- this test isolates fill
        // mode.
        const std::array<RasterStateTriangleVertex, 3> vertices{
            RasterStateTriangleVertex{{ 0.0f,  0.5f, 0.0f}, 0.0f, {1.0f, 0.0f, 0.0f, 1.0f}}, // top, red
            RasterStateTriangleVertex{{-0.5f, -0.5f, 0.0f}, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f}}, // bottom-left, blue
            RasterStateTriangleVertex{{ 0.5f, -0.5f, 0.0f}, 0.0f, {0.0f, 1.0f, 0.0f, 1.0f}}, // bottom-right, green
        };
        const std::array<uint32_t, 3> indices{0, 1, 2};

        vkm::VkmBufferInfo vertexBufferInfo{};
        vertexBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                                  vkm::VkmResourceCreateInfo::AllowTransferDst |
                                  vkm::VkmResourceCreateInfo::AllowTransferSrc;
        vertexBufferInfo._size = sizeof(vertices);
        vertexBufferInfo._placementHint = vkm::VkmMemoryPlacementHint::Committed;
        vertexBufferInfo._debugName = "RasterStateVertexBuffer";
        vkm::VkmBuffer* vertexBuffer = driver->newBuffer(vertexBufferInfo);
        REQUIRE(vertexBuffer != nullptr);
        REQUIRE(driver->uploadToBuffer(vertexBuffer->getHandle(), vertices.data(), sizeof(vertices)));

        vkm::VkmBufferInfo indexBufferInfo{};
        indexBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                                 vkm::VkmResourceCreateInfo::AllowTransferDst |
                                 vkm::VkmResourceCreateInfo::AllowTransferSrc;
        indexBufferInfo._size = sizeof(indices);
        indexBufferInfo._placementHint = vkm::VkmMemoryPlacementHint::Committed;
        indexBufferInfo._debugName = "RasterStateIndexBuffer";
        vkm::VkmBuffer* indexBuffer = driver->newBuffer(indexBufferInfo);
        REQUIRE(indexBuffer != nullptr);
        REQUIRE(driver->uploadToBuffer(indexBuffer->getHandle(), indices.data(), sizeof(indices)));

        const uint32_t vertexSlot =
            bindlessManager->registerBuffer(vertexBuffer->getHandle(), vkm::VkmBindlessArrayType::Buffer);
        const uint32_t indexSlot =
            bindlessManager->registerBuffer(indexBuffer->getHandle(), vkm::VkmBindlessArrayType::IndexBuffer);
        REQUIRE(vertexSlot != UINT32_MAX);
        REQUIRE(indexSlot != UINT32_MAX);

        vkm::VkmTextureInfo texInfo{};
        texInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc;
        texInfo._extent = glm::uvec3(kSize, kSize, 1);
        texInfo._format = vkm::VkmFormat::BGRA8_UNORM;
        texInfo._numMipLevels = 1;
        texInfo._numArrayLayers = 1;
        vkm::VkmTexture* offscreen = driver->newTexture(texInfo);
        REQUIRE(offscreen != nullptr);

        // Clear to opaque black: _clearColors defaults to 0, only alpha is set.
        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = kSize;
        fbDesc._height = kSize;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        fbDesc._colorAttachments[0] = offscreen->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        const uint32_t pushConstants[2] = {vertexSlot, indexSlot};
        subGraph->setRenderCallback([pso, pushConstants](vkm::VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            commandBuffer->setPushConstants(pushConstants, sizeof(pushConstants));
            commandBuffer->draw(3, 1, 0, 0);
        });
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        vkm::VkmTextureReadbackResult readback = driver->readbackTexture(offscreen->getHandle());
        REQUIRE(readback.pixels.size() == static_cast<size_t>(kSize) * kSize * 4);
        REQUIRE(readback.channels == 4);

        const auto pixelAt = [&](uint32_t x, uint32_t y) {
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };

        // NDC->framebuffer for a 64x64 target under the +Y-up convention:
        //   apex        ( 0.0,  0.5) -> (32, 16)
        //   bottom-left (-0.5, -0.5) -> (16, 48)
        //   bottom-right( 0.5, -0.5) -> (48, 48)
        // At y=40 the left edge sits at x=20 and the right edge at x=44, and the bottom edge is
        // at y=48, so (32, 40) is at least 8px clear of every edge -- solidly interior, and
        // safely outside any line-rasterization width.
        {
            const uint8_t* interior = pixelAt(32, 40);
            CHECK(interior[0] == 0);
            CHECK(interior[1] == 0);
            CHECK(interior[2] == 0);
        }

        // Guards the check above against passing vacuously on an empty image: the perimeter is
        // roughly 150px, so a drawn outline lights far more than 30.
        {
            size_t litPixels = 0;
            for (uint32_t y = 0; y < kSize; ++y)
            {
                for (uint32_t x = 0; x < kSize; ++x)
                {
                    const uint8_t* pixel = pixelAt(x, y);
                    if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0)
                    {
                        ++litPixels;
                    }
                }
            }
            CHECK(litPixels > 30);
        }

        bindlessManager->unregisterBuffer(vertexSlot, vkm::VkmBindlessArrayType::Buffer);
        bindlessManager->unregisterBuffer(indexSlot, vkm::VkmBindlessArrayType::IndexBuffer);

        // All GPU work above is already retired (ensureCompleted + readbackTexture's waitIdle),
        // so these are safe to free here. Required, not merely tidy: the Vulkan fixture calls
        // driver->destroy(), and VMA asserts "Unfreed dedicated allocations found!" if
        // Committed resources outlive the allocator.
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();
        resourcePool->releaseResource(offscreen->getHandle());
        resourcePool->releaseResource(indexBuffer->getHandle());
        resourcePool->releaseResource(vertexBuffer->getHandle());
    }
} // namespace vkmtest

#endif // TEST_RASTER_STATE_SHARED_HPP
