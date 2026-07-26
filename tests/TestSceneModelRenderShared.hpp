#ifndef TEST_SCENE_MODEL_RENDER_SHARED_HPP
#define TEST_SCENE_MODEL_RENDER_SHARED_HPP

// Body of the glTF scene-rendering test. Currently only the Metal fixture drives it (see
// TestSceneModelRenderMetal.mm): like TestMetalBindlessTriangle, it verifies real
// pixel output, which is only checked on the backend this machine can run. The Vulkan
// depth-attachment path it exercises is validated through the model_viewer sample on
// Vulkan hardware instead.
//
// What this proves, end to end and without a window: a glTF file imports into VkmSceneModel,
// VkmScene pools its geometry and publishes the per-object / per-frame records into the bindless
// set, and the model_viewer PSO permutation for that vertex layout draws it into an offscreen
// color target with a real depth attachment bound -- the same path src/samples/model_viewer takes.
//
// The rendered pixel carries the material's base color, which the shader reads out of the scene's
// material pool through the ObjectData record SV_InstanceID selects. So a broken ObjectData layout,
// a wrong pool offset, or a firstInstance that does not reach the shader all show up as a wrong
// color rather than as nothing at all.
//
// It is also a GPU-level check of descriptor set 1: the camera travels through the engine's
// per-frame constants, so a broken set-1 binding shows up as a blank target.

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/scene_model.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vkmtest
{
    constexpr uint32_t kSceneModelTargetSize = 64;

    /*
    * @brief Renders `scene` into `fbDesc`'s targets with `frameData` and returns the readback.
    *
    * The per-frame update and the draws go into one render graph, the update recorded first so its
    * copies land ahead of the draws that read them.
    */
    inline vkm::VkmTextureReadbackResult renderScene(vkm::VkmDriverBase* driver,
                                                     vkm::VkmScene& scene,
                                                     vkm::VkmPipelineStateBase* pso,
                                                     const glm::mat4& viewProjection,
                                                     const vkm::VkmFrameData& frameData,
                                                     const vkm::VkmFrameBufferDescriptor& fbDesc)
    {
        std::vector<vkm::VkmResourceHandle> referenced;
        scene.collectReferencedResources(&referenced);

        // Frame slot 0, matching the render graph below. Only _viewProjection is read by this
        // PSO; the rest stay identity.
        vkm::VkmFrameConstants frameConstants{};
        frameConstants._viewProjection = viewProjection;
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);

        auto* updateSubGraph = renderGraph.beginTransferSubGraph("SceneUpdate");
        for (vkm::VkmResourceHandle handle : referenced)
        {
            updateSubGraph->addReferencedResource(handle);
        }
        updateSubGraph->setTransferCallback([&scene, &frameData](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData);
        });

        auto* cullSubGraph = renderGraph.beginComputeSubGraph("SceneCull");
        for (vkm::VkmResourceHandle handle : referenced)
        {
            cullSubGraph->addReferencedResource(handle);
        }
        cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordCull(commandBuffer);
        });

        auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        for (vkm::VkmResourceHandle handle : referenced)
        {
            subGraph->addReferencedResource(handle);
        }
        subGraph->setRenderCallback([&scene, pso](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordDrawBatches(commandBuffer, [pso](const vkm::VkmScene::DrawBatch&) { return pso; });
        });

        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        return driver->readbackTexture(fbDesc._colorAttachments[0]);
    }

    // Asserts the material color reached the pixels of a scene rendered head-on.
    inline void checkSceneModelPixels(const vkm::VkmTextureReadbackResult& readback)
    {
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

    // Asserts nothing at all was rasterized, i.e. the target still holds the clear color.
    inline void checkSceneTargetIsClear(const vkm::VkmTextureReadbackResult& readback)
    {
        REQUIRE(readback.pixels.size() ==
                static_cast<size_t>(kSceneModelTargetSize) * kSceneModelTargetSize * 4);

        size_t litPixels = 0;
        for (size_t i = 0; i + 2 < readback.pixels.size(); i += readback.channels)
        {
            if (readback.pixels[i] != 0 || readback.pixels[i + 1] != 0 || readback.pixels[i + 2] != 0)
            {
                ++litPixels;
            }
        }
        CHECK(litPixels == 0);
    }

    /*
    * @brief Reads back how many objects of `batchIndex` survived the last culling pass.
    *
    * The rendered pixels cannot prove culling on their own: the frustum planes come from the same
    * view-projection the rasterizer clips against, so anything culling rejects would have been
    * clipped anyway. The count the culling pass wrote is the only externally observable difference.
    */
    inline uint32_t readVisibleCount(vkm::VkmDriverBase* driver, const vkm::VkmScene& scene, size_t batchIndex)
    {
        const vkm::VkmScene::DrawBatch& batch = scene.getDrawBatches()[batchIndex];

        vkm::VkmStagingBufferInfo stagingInfo{};
        stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
        stagingInfo._size = sizeof(uint32_t);
        stagingInfo._debugName = "SceneVisibleCountReadback";
        vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
        REQUIRE(staging != nullptr);

        const vkm::VkmResourceHandle argumentBuffer = scene.getArgumentBuffer();
        const vkm::VkmResourceHandle destination = staging->getHandle();
        const uint64_t countOffset = static_cast<uint64_t>(batch._countWordOffset) * sizeof(uint32_t);

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginTransferSubGraph("VisibleCountReadback");
        subGraph->addReferencedResource(argumentBuffer);
        subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
            commandBuffer->copyBuffer(argumentBuffer, destination, countOffset, 0, sizeof(uint32_t));
        });
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        uint32_t visibleCount = 0;
        staging->invalidate(0, sizeof(uint32_t));
        std::memcpy(&visibleCount, staging->map(), sizeof(uint32_t));
        driver->getDeferredReclaimer()->requestRelease(destination);
        return visibleCount;
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

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE(manager.loadPipelineStatesFromDirectory(TEST_MODEL_VIEWER_SAMPLE_DIR, TEST_MODEL_VIEWER_SHADER_CACHE_DIR,
                                                        vkm::VkmPipelineStateOrigin::User, &psoError));
        // The scene's culling and emit compute passes are engine PSOs, which VkmEngine would
        // normally load; this fixture drives a bare driver, so it loads them itself.
        REQUIRE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR, TEST_ENGINE_SHADER_CACHE_DIR,
                                                        vkm::VkmPipelineStateOrigin::Engine, &psoError));

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        REQUIRE(scene.build(driver, &manager, &error));
        REQUIRE(scene.getObjects().size() == 1);
        REQUIRE(scene.getDrawBatches().size() == 1);
        REQUIRE(scene.getTotalIndexCount() == 3);
        // The fixture imports as StandardPBR, so that is the permutation that must be bound.
        const std::string psoName =
            std::string("model_viewer_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState(psoName, vkm::VkmPipelineStateOrigin::User);
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

        /*
        * The fixture's node carries a (1, 2, 3) translation, and VkmScene honours the node
        * hierarchy, so the placed object does not sit at the origin. Reset it to identity through
        * the per-object update path -- which is also what exercises the dirty-range upload -- so
        * the view-projection below is the whole transform.
        */
        scene.setObjectTransform(0, glm::mat4(1.0f));

        // The fixture triangle spans x,y in [0,1] at z = 0, so this maps it onto the lower
        // left half of clip space at depth 0.5 (in front of the 1.0 depth clear), wound
        // counter-clockwise in the engine's +Y-up convention -- i.e. front-facing for the
        // PSO's back-face culling.
        const glm::mat4 viewProjection = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.5f)) *
                                         glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);
        // Head-on with the fixture's +Z normals, so the shading term saturates to 1 and the
        // material's base color (0.25, 0.5, 0.75) reaches the pixels unattenuated.
        frameData._lightDirection = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);

        checkSceneModelPixels(renderScene(driver, scene, pso, viewProjection, frameData, fbDesc));

        // The object is in view, so culling must have kept it -- and the pixels above prove the
        // whole chain (cull compaction -> emit -> indirect fetch) produced a live draw record,
        // because an all-zero argument record would have drawn nothing at all.
        CHECK(readVisibleCount(driver, scene, 0) == 1);

        /*
        * Now move it far behind the camera. The geometry, pipeline and draw call are unchanged, so
        * an empty target alone would prove nothing (the rasterizer would have clipped it anyway) --
        * the visible count dropping to zero is what shows the cull dispatch actually rejected it,
        * its compaction landed, and the emit pass zeroed the slot.
        */
        scene.setObjectTransform(0, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1000.0f)));
        checkSceneTargetIsClear(renderScene(driver, scene, pso, viewProjection, frameData, fbDesc));
        CHECK(readVisibleCount(driver, scene, 0) == 0);

        // Back in view: the same scene must draw again, so culling is not a one-way latch and the
        // per-frame count reset works.
        scene.setObjectTransform(0, glm::mat4(1.0f));
        checkSceneModelPixels(renderScene(driver, scene, pso, viewProjection, frameData, fbDesc));
        CHECK(readVisibleCount(driver, scene, 0) == 1);

        // The bindless slot-recycling that the Scene Browser's scene-swap depends on is
        // covered headlessly by "slots are recycled after unregister" (TestMetalBindlessTriangle)
        // and by the snapshot aggregation test, so it is not re-driven through a second GPU
        // render here.

        scene.destroy(driver);
    }
} // namespace vkmtest

#endif // TEST_SCENE_MODEL_RENDER_SHARED_HPP
