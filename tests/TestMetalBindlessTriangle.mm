#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>

#include <array>
#include <cstdint>
#include <memory>

namespace
{
    // Matches triangle.hlsl's VertexData struct (see src/samples/triangle/main.cpp for the
    // padding rationale).
    struct TriangleVertex
    {
        float position[3];
        float _pad0;
        float color[4];
    };

    struct MetalBindlessFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalBindlessFixture()
        {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device == nil)
            {
                initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported,
                                                "No Metal device available on this system."};
                return;
            }
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::make_unique<vkm::VkmDriverMetal>(device);
            initResult = driver->initialize(&opts);
        }
    };
}

TEST_CASE("Metal bindless - registerBuffer returns valid slots and draw path executes")
{
    MetalBindlessFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver.get();

    // The engine-global bindless manager must exist right after driver init.
    vkm::VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
    REQUIRE(bindlessManager != nullptr);

    vkm::VkmPipelineStateManager manager(driver);
    std::string err;
    REQUIRE(manager.loadPipelineStatesFromDirectory(TEST_TRIANGLE_SAMPLE_DIR, TEST_TRIANGLE_SHADER_CACHE_DIR,
                                                    vkm::VkmPipelineStateOrigin::User, &err));
    vkm::VkmPipelineStateBase* pso = manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(pso != nullptr);

    const std::array<TriangleVertex, 3> vertices{
        TriangleVertex{{ 0.0f, -0.5f, 0.0f}, 0.0f, {1.0f, 0.0f, 0.0f, 1.0f}},
        TriangleVertex{{ 0.5f,  0.5f, 0.0f}, 0.0f, {0.0f, 1.0f, 0.0f, 1.0f}},
        TriangleVertex{{-0.5f,  0.5f, 0.0f}, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f}},
    };
    const std::array<uint32_t, 3> indices{0, 1, 2};

    vkm::VkmBufferInfo vertexBufferInfo{};
    vertexBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                              vkm::VkmResourceCreateInfo::AllowTransferDst |
                              vkm::VkmResourceCreateInfo::AllowTransferSrc;
    vertexBufferInfo._size = sizeof(vertices);
    vertexBufferInfo._placementHint = vkm::VkmMemoryPlacementHint::ForceCommitted;
    vertexBufferInfo._debugName = "TestTriangleVertexBuffer";
    vkm::VkmBuffer* vertexBuffer = driver->newBuffer(vertexBufferInfo);
    REQUIRE(vertexBuffer != nullptr);
    // Exercises VkmCommandBufferMetal::onCopyBuffer (staging -> device copy).
    REQUIRE(driver->uploadToBuffer(vertexBuffer->getHandle(), vertices.data(), sizeof(vertices)));

    vkm::VkmBufferInfo indexBufferInfo{};
    indexBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                             vkm::VkmResourceCreateInfo::AllowTransferDst |
                             vkm::VkmResourceCreateInfo::AllowTransferSrc;
    indexBufferInfo._size = sizeof(indices);
    indexBufferInfo._placementHint = vkm::VkmMemoryPlacementHint::ForceCommitted;
    indexBufferInfo._debugName = "TestTriangleIndexBuffer";
    vkm::VkmBuffer* indexBuffer = driver->newBuffer(indexBufferInfo);
    REQUIRE(indexBuffer != nullptr);
    REQUIRE(driver->uploadToBuffer(indexBuffer->getHandle(), indices.data(), sizeof(indices)));

    const uint32_t vertexSlot = bindlessManager->registerBuffer(vertexBuffer->getHandle(), vkm::VkmBindlessArrayType::Buffer);
    const uint32_t indexSlot = bindlessManager->registerBuffer(indexBuffer->getHandle(), vkm::VkmBindlessArrayType::IndexBuffer);
    REQUIRE(vertexSlot != UINT32_MAX);
    REQUIRE(indexSlot != UINT32_MAX);

    // Offscreen target matching the PSO's declared bgra8_unorm color attachment.
    vkm::VkmTextureInfo texInfo{};
    texInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc;
    texInfo._extent = glm::uvec3(64, 64, 1);
    texInfo._format = vkm::VkmFormat::BGRA8_UNORM;
    texInfo._numMipLevels = 1;
    texInfo._numArrayLayers = 1;
    vkm::VkmTexture* offscreen = driver->newTexture(texInfo);
    REQUIRE(offscreen != nullptr);

    vkm::VkmFrameBufferDescriptor fbDesc{};
    fbDesc._width = 64;
    fbDesc._height = 64;
    fbDesc._renderPass._colorAttachmentCount = 1;
    fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
    fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
    fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
    fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
    fbDesc._colorAttachments[0] = offscreen->getHandle();

    // Drives the full bindless draw path (bindPipeline's argument-table setup,
    // setPushConstants' ring allocation, drawPrimitives) through engine abstractions. The
    // Metal validation layer (enabled by the fixture) turns any binding/residency mistake
    // into a test-visible error.
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

    // TODO: compare center/corner pixels once VkmDriverBase::readbackTexture() exists
    // (see TestBackbufferReadback.mm for the agreed interface) -- until then this test
    // verifies the draw path records/submits cleanly under the validation layer.

    bindlessManager->unregisterBuffer(vertexSlot, vkm::VkmBindlessArrayType::Buffer);
    bindlessManager->unregisterBuffer(indexSlot, vkm::VkmBindlessArrayType::IndexBuffer);
}

TEST_CASE("Metal bindless - slots are recycled after unregister")
{
    MetalBindlessFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkm::VkmDriverBase* driver = f.driver.get();

    vkm::VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
    REQUIRE(bindlessManager != nullptr);

    vkm::VkmBufferInfo bufferInfo{};
    bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite |
                        vkm::VkmResourceCreateInfo::AllowTransferDst |
                        vkm::VkmResourceCreateInfo::AllowTransferSrc;
    bufferInfo._size = 64;
    bufferInfo._placementHint = vkm::VkmMemoryPlacementHint::ForceCommitted;
    bufferInfo._debugName = "TestSlotRecycleBuffer";
    vkm::VkmBuffer* buffer = driver->newBuffer(bufferInfo);
    REQUIRE(buffer != nullptr);

    const uint32_t firstSlot = bindlessManager->registerBuffer(buffer->getHandle(), vkm::VkmBindlessArrayType::Buffer);
    REQUIRE(firstSlot != UINT32_MAX);
    bindlessManager->unregisterBuffer(firstSlot, vkm::VkmBindlessArrayType::Buffer);

    const uint32_t secondSlot = bindlessManager->registerBuffer(buffer->getHandle(), vkm::VkmBindlessArrayType::Buffer);
    CHECK(secondSlot == firstSlot);
    bindlessManager->unregisterBuffer(secondSlot, vkm::VkmBindlessArrayType::Buffer);
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
