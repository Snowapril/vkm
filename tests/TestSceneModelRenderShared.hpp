#ifndef TEST_SCENE_MODEL_RENDER_SHARED_HPP
#define TEST_SCENE_MODEL_RENDER_SHARED_HPP

// Body of the glTF scene-rendering test. Currently only the Metal fixture drives it (see
// TestSceneModelRenderMetal.mm): like TestMetalBindlessTriangle, it verifies real
// pixel output, which is only checked on the backend this machine can run. The Vulkan
// depth-attachment path it exercises is validated through the model_viewer sample on
// Vulkan hardware instead.
//
// What this proves, end to end and without a window: a glTF file imports into
// VkmSceneModel, VkmSceneModelGpu turns it into bindless vertex/index buffers, and the
// model_viewer PSO draws it into an offscreen color target with a real depth attachment
// bound -- the same path src/samples/model_viewer takes. The rendered pixel also carries
// the material's base color, so a broken material or push-constant layout shows up as a
// wrong color rather than as nothing at all.
//
// It is also the engine's only GPU-level check of descriptor set 1 (the per-frame camera
// constants -- see renderer/backend/common/frame_constants.h): the transform that places the
// triangle is split so set 1 supplies the view-projection half and the push constants supply
// the model half, which means a set-1 binding that is unbound, mis-offset or transposed moves
// the geometry off the sampled pixels instead of quietly still working.

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene_model.h>
#include <vkm/renderer/scene/scene_model_gpu.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>

namespace vkmtest
{
    constexpr uint32_t kSceneModelTargetSize = 64;

    // Mirrors PushConstants in src/samples/model_viewer/model_viewer.hlsl.
    struct SceneModelPushConstants
    {
        glm::mat4 _model;
        uint32_t _vertexBufferSlot;
        uint32_t _indexBufferSlot;
        float _pad0[2];
        glm::vec4 _baseColor;
        glm::vec4 _lightDirectionObjectSpace;
    };

    // Draws one mesh into `fbDesc`'s targets and asserts the material color reached the
    // pixels. `viewProjection` goes through descriptor set 1 and `model` through the push
    // constants, exactly as the sample does.
    inline void renderSceneModelAndCheckPixels(vkm::VkmDriverBase* driver,
                                               vkm::VkmPipelineStateBase* pso,
                                               const vkm::VkmSceneModelGpu::MeshGpu& meshGpu,
                                               const glm::vec4& baseColor,
                                               const glm::mat4& viewProjection,
                                               const glm::mat4& model,
                                               const vkm::VkmFrameBufferDescriptor& fbDesc)
    {
        SceneModelPushConstants pushConstants{};
        pushConstants._model = model;
        pushConstants._vertexBufferSlot = meshGpu._vertexBufferSlot;
        pushConstants._indexBufferSlot = meshGpu._indexBufferSlot;
        pushConstants._baseColor = baseColor;
        // Head-on with the fixture's +Z normals, so the shading term saturates to 1.
        pushConstants._lightDirectionObjectSpace = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);

        // Frame slot 0, matching the render graph below. Only _viewProjection is read by this
        // PSO; the rest stay identity.
        vkm::VkmFrameConstants frameConstants{};
        frameConstants._viewProjection = viewProjection;
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        subGraph->addReferencedResource(meshGpu._vertexBuffer);
        subGraph->addReferencedResource(meshGpu._indexBuffer);
        subGraph->setRenderCallback([pso, pushConstants, &meshGpu](vkm::VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            commandBuffer->setPushConstants(&pushConstants, sizeof(SceneModelPushConstants));
            commandBuffer->draw(meshGpu._indexCount, 1, 0, 0);
        });
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        vkm::VkmTextureReadbackResult readback = driver->readbackTexture(fbDesc._colorAttachments[0]);
        REQUIRE(readback.pixels.size() ==
                static_cast<size_t>(kSceneModelTargetSize) * kSceneModelTargetSize * 4);

        const auto pixelAt = [&](uint32_t x, uint32_t y) {
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };

        // BGRA8: byte order B,G,R,A. The upper-right corner is outside the triangle, so it
        // must still hold the clear color.
        const uint8_t* outside = pixelAt(kSceneModelTargetSize - 4, 3);
        CHECK(outside[0] == 0);
        CHECK(outside[1] == 0);
        CHECK(outside[2] == 0);

        // NDC (-0.5, -0.5) is inside the triangle; +Y-up clip space puts it in the lower
        // half of the image, i.e. towards the last rows of the readback.
        const uint8_t* inside = pixelAt(kSceneModelTargetSize / 4, kSceneModelTargetSize * 3 / 4);
        // baseColorFactor is (0.25, 0.5, 0.75) with the shading term at 1.0, so the
        // channels must come out strictly ordered blue > green > red > 0. Exact values are
        // left alone: they depend on the target format's transfer function.
        CHECK(inside[2] > 0);
        CHECK(inside[1] > inside[2]);
        CHECK(inside[0] > inside[1]);
    }

    inline void runSceneModelRenderTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf",
                                     &model, &error, importOptions));
        REQUIRE(model._meshes.size() == 1);
        REQUIRE(model._materials.size() == 1);

        vkm::VkmSceneModelGpu modelGpu;
        REQUIRE(modelGpu.upload(driver, model, &error));
        REQUIRE(modelGpu.getMeshes()[0]._indexCount == 3);

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE(manager.loadPipelineStatesFromDirectory(TEST_MODEL_VIEWER_SAMPLE_DIR, TEST_MODEL_VIEWER_SHADER_CACHE_DIR,
                                                        vkm::VkmPipelineStateOrigin::User, &psoError));
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState("model_viewer_pso[default]",
                                                                  vkm::VkmPipelineStateOrigin::User);
        REQUIRE(pso != nullptr);

        vkm::VkmTextureInfo colorInfo{};
        colorInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc;
        colorInfo._extent = glm::uvec3(kSceneModelTargetSize, kSceneModelTargetSize, 1);
        colorInfo._format = driver->getSwapChainColorFormat(); // what "swapchain" in the PSO resolves to
        colorInfo._numMipLevels = 1;
        colorInfo._numArrayLayers = 1;
        colorInfo._debugName = "SceneModelColorTarget";
        vkm::VkmTexture* colorTarget = driver->newTexture(colorInfo);
        REQUIRE(colorTarget != nullptr);

        vkm::VkmTextureInfo depthInfo{};
        depthInfo._flags = vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment;
        depthInfo._extent = glm::uvec3(kSceneModelTargetSize, kSceneModelTargetSize, 1);
        depthInfo._format = vkm::VkmFormat::D32_SFLOAT; // matches renderpass.json
        depthInfo._numMipLevels = 1;
        depthInfo._numArrayLayers = 1;
        depthInfo._debugName = "SceneModelDepthTarget";
        vkm::VkmTexture* depthTarget = driver->newTexture(depthInfo);
        REQUIRE(depthTarget != nullptr);

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = kSceneModelTargetSize;
        fbDesc._height = kSceneModelTargetSize;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f; // opaque black
        fbDesc._colorAttachments[0] = colorTarget->getHandle();

        vkm::VkmDepthStencilAttachmentDescriptor depthDesc{};
        depthDesc._attachmentId = 0;
        depthDesc._loadAction = vkm::VkmLoadAction::Clear;
        depthDesc._storeAction = vkm::VkmStoreAction::Store;
        depthDesc._clearDepth = 1.0f;
        depthDesc._clearStencil = 0;
        fbDesc._renderPass._depthStencilAttachment = depthDesc;
        fbDesc._depthStencilAttachment = depthTarget->getHandle();

        // The fixture triangle spans x,y in [0,1] at z = 0, so the product of these two maps
        // it onto the lower left half of clip space at depth 0.5 (in front of the 1.0 depth
        // clear), wound counter-clockwise in the engine's +Y-up convention -- i.e.
        // front-facing for the PSO's back-face culling. Deliberately split across the two
        // paths so neither is an identity that could mask a broken binding: the translate
        // travels through descriptor set 1, the scale through the push constants.
        const glm::mat4 viewProjection = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.5f));
        const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
        const glm::vec4 baseColor = model._materials[0]._baseColorFactor; // 0.25, 0.5, 0.75, 1

        renderSceneModelAndCheckPixels(driver, pso, modelGpu.getMeshes()[0], baseColor,
                                       viewProjection, modelMatrix, fbDesc);

        // The bindless slot-recycling that the Scene Browser's scene-swap depends on is
        // covered headlessly by "slots are recycled after unregister" (TestMetalBindlessTriangle)
        // and by the snapshot aggregation test, so it is not re-driven through a second GPU
        // render here.

        modelGpu.destroy(driver);
    }
} // namespace vkmtest

#endif // TEST_SCENE_MODEL_RENDER_SHARED_HPP
