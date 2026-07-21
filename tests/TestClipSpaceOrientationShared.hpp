#ifndef TEST_CLIP_SPACE_ORIENTATION_SHARED_HPP
#define TEST_CLIP_SPACE_ORIENTATION_SHARED_HPP

// Backend-agnostic body of the clip-space orientation regression test. Only the driver
// fixture is backend-specific (Metal needs MTLCreateSystemDefaultDevice() from
// Objective-C++), so the render + assertion logic lives here and is called from both
// TestClipSpaceOrientation.cpp (Vulkan) and TestClipSpaceOrientationMetal.mm (Metal).
//
// What this proves: one HLSL shader is cross-compiled to SPIR-V/MSL/WGSL, so every backend
// receives identical clip-space vertices. The engine convention is +Y up (HLSL/D3D, Metal,
// WebGPU); Vulkan's NDC is +Y down, so the Vulkan backend binds a negative-height viewport
// to compensate. Removing that flip renders the triangle upside down, which the
// position-based colour assertions below detect.

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
    struct ClipSpaceTriangleVertex
    {
        float position[3];
        float _pad0;
        float color[4];
    };

    // Renders the triangle sample's PSO into a 64x64 offscreen BGRA8 target and asserts the
    // resulting image orientation. All GPU work goes through engine abstractions only (see
    // tests/CLAUDE.md).
    inline void runClipSpaceOrientationTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kSize = 64;

        vkm::VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        REQUIRE(bindlessManager != nullptr);

        vkm::VkmPipelineStateManager manager(driver);
        std::string err;
        REQUIRE(manager.loadPipelineStatesFromDirectory(TEST_TRIANGLE_SAMPLE_DIR, TEST_TRIANGLE_SHADER_CACHE_DIR,
                                                        vkm::VkmPipelineStateOrigin::User, &err));
        vkm::VkmPipelineStateBase* pso =
            manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User);
        REQUIRE(pso != nullptr);

        // Mirrors src/samples/triangle/main.cpp exactly: clip-space, +Y up, wound
        // counter-clockwise. Keep the two in sync -- this test is meaningless if the sample
        // changes orientation and this copy does not.
        const std::array<ClipSpaceTriangleVertex, 3> vertices{
            ClipSpaceTriangleVertex{{ 0.0f,  0.5f, 0.0f}, 0.0f, {1.0f, 0.0f, 0.0f, 1.0f}}, // top, red
            ClipSpaceTriangleVertex{{-0.5f, -0.5f, 0.0f}, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f}}, // bottom-left, blue
            ClipSpaceTriangleVertex{{ 0.5f, -0.5f, 0.0f}, 0.0f, {0.0f, 1.0f, 0.0f, 1.0f}}, // bottom-right, green
        };
        const std::array<uint32_t, 3> indices{0, 1, 2};

        vkm::VkmBufferInfo vertexBufferInfo{};
        vertexBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                                  vkm::VkmResourceCreateInfo::AllowTransferDst |
                                  vkm::VkmResourceCreateInfo::AllowTransferSrc;
        vertexBufferInfo._size = sizeof(vertices);
        vertexBufferInfo._placementHint = vkm::VkmMemoryPlacementHint::ForceCommitted;
        vertexBufferInfo._debugName = "ClipSpaceVertexBuffer";
        vkm::VkmBuffer* vertexBuffer = driver->newBuffer(vertexBufferInfo);
        REQUIRE(vertexBuffer != nullptr);
        REQUIRE(driver->uploadToBuffer(vertexBuffer->getHandle(), vertices.data(), sizeof(vertices)));

        vkm::VkmBufferInfo indexBufferInfo{};
        indexBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                                 vkm::VkmResourceCreateInfo::AllowTransferDst |
                                 vkm::VkmResourceCreateInfo::AllowTransferSrc;
        indexBufferInfo._size = sizeof(indices);
        indexBufferInfo._placementHint = vkm::VkmMemoryPlacementHint::ForceCommitted;
        indexBufferInfo._debugName = "ClipSpaceIndexBuffer";
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

        // Readback is row-major with row 0 = the top row of the framebuffer: the copy is a
        // whole-image blit starting at texel offset (0,0) with no row reordering (see
        // VkmCommandBufferVulkan::onCopyTextureToBuffer / the Metal equivalent).
        // Format is BGRA8, so byte order is B,G,R,A.
        const auto pixelAt = [&](uint32_t x, uint32_t y) {
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };

        // NDC->framebuffer for a 64x64 target under the +Y-up convention:
        //   apex        ( 0.0,  0.5) -> (32, 16)
        //   bottom-left (-0.5, -0.5) -> (16, 48)
        //   bottom-right( 0.5, -0.5) -> (48, 48)
        // Sample points sit comfortably inside the triangle, away from every edge.
        {
            const uint8_t* topCentre = pixelAt(32, 22);
            CHECK(topCentre[2] > topCentre[0]); // R > B
            CHECK(topCentre[2] > topCentre[1]); // R > G
        }
        {
            const uint8_t* bottomLeft = pixelAt(21, 45);
            CHECK(bottomLeft[0] > bottomLeft[1]); // B > G
            CHECK(bottomLeft[0] > bottomLeft[2]); // B > R
        }
        {
            const uint8_t* bottomRight = pixelAt(43, 45);
            CHECK(bottomRight[1] > bottomRight[0]); // G > B
            CHECK(bottomRight[1] > bottomRight[2]); // G > R
        }
        {
            // Top-left corner is outside the triangle under either orientation only if the
            // image is correct; keep it as the clear-colour sanity check.
            const uint8_t* corner = pixelAt(1, 1);
            CHECK(corner[0] == 0);
            CHECK(corner[1] == 0);
            CHECK(corner[2] == 0);
            CHECK(corner[3] == 255);
        }
        {
            // Coverage-only mirror detector, independent of vertex colours. Row 18 is close
            // to the apex, where the triangle is only ~2px wide around x=32, so (19, 18)
            // must be background. Under a vertical flip that same pixel lands in the wide
            // part of the triangle and is covered -- this check fails on a mirrored image
            // even if every vertex were the same colour.
            const uint8_t* nearApexLeft = pixelAt(19, 18);
            CHECK(nearApexLeft[0] == 0);
            CHECK(nearApexLeft[1] == 0);
            CHECK(nearApexLeft[2] == 0);
        }

        bindlessManager->unregisterBuffer(vertexSlot, vkm::VkmBindlessArrayType::Buffer);
        bindlessManager->unregisterBuffer(indexSlot, vkm::VkmBindlessArrayType::IndexBuffer);

        // All GPU work above is already retired (ensureCompleted + readbackTexture's
        // waitIdle), so these are safe to free here. Required, not merely tidy: the Vulkan
        // fixture calls driver->destroy(), and VMA asserts "Unfreed dedicated allocations
        // found!" if ForceCommitted resources outlive the allocator.
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();
        resourcePool->releaseResource(offscreen->getHandle());
        resourcePool->releaseResource(indexBuffer->getHandle());
        resourcePool->releaseResource(vertexBuffer->getHandle());
    }
} // namespace vkmtest

#endif // TEST_CLIP_SPACE_ORIENTATION_SHARED_HPP
