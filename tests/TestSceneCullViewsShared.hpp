#ifndef TEST_SCENE_CULL_VIEWS_SHARED_HPP
#define TEST_SCENE_CULL_VIEWS_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cstring>
#include <string>

namespace vkmtest
{
    /*
    * @brief How many objects of batch 0 survived the cull recorded for `viewIndex`.
    *
    * Reads the count word the cull pass wrote. Rendered pixels cannot show this: the frustum
    * planes come from the same view-projection the rasterizer clips against, so anything culling
    * rejects would have been clipped anyway.
    */
    inline uint32_t readVisibleCountForView(vkm::VkmDriverBase* driver, const vkm::VkmScene& scene,
                                            uint32_t viewIndex)
    {
        const uint32_t batchCount = static_cast<uint32_t>(scene.getDrawBatches().size());
        const vkm::VkmScene::DrawBatch& batch = scene.getDrawBatches()[0];
        // Counts are packed view-major at the front of the buffer; see VkmScene's buffer comment.
        const uint64_t countOffset =
            static_cast<uint64_t>(batch._countWordOffset + viewIndex * batchCount) * sizeof(uint32_t);

        vkm::VkmStagingBufferInfo stagingInfo{};
        stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
        stagingInfo._size = sizeof(uint32_t);
        stagingInfo._debugName = "SceneCullViewCountReadback";
        vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
        REQUIRE(staging != nullptr);

        const vkm::VkmResourceHandle argumentBuffer = scene.getArgumentBuffer();
        const vkm::VkmResourceHandle destination = staging->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginTransferSubGraph("CullViewCountReadback");
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

    /*
    * @brief Two views culled in one frame keep their own frusta and their own results.
    *
    * @details This is what a GI frame needs: a probe capture sees in every direction, so culling it
    * against the camera's frustum would drop exactly the geometry behind the camera that indirect
    * light comes from. One frame therefore has to cull twice, against different frusta.
    *
    * Three separate things have to hold for that, and one assertion covers all three because each
    * failure collapses the two counts onto the same value:
    *   - each view's frame data survives to its own dispatch. Both recordUpdate() calls write host
    *     memory immediately, long before either GPU copy runs, so a shared staging region would
    *     leave the first cull reading the second view's frustum;
    *   - the cull shader indexes the frame data by the pushed view, rather than always reading 0;
    *   - the second cull clears and fills only its own count words, leaving the first cull's intact.
    */
    inline void runSceneCullViewsTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf",
                                     &model, &error, importOptions));

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR, TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        REQUIRE(scene.build(driver, &manager, &error));
        REQUIRE(scene.getObjects().size() == 1);
        REQUIRE(scene.getDrawBatches().size() == 1);
        scene.setObjectTransform(0, glm::mat4(1.0f));

        // The fixture triangle spans x,y in [0,1] at z = 0. One camera in front of it, one behind
        // and facing away, so the same object is inside exactly one of the two frusta.
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 50.0f);
        const glm::mat4 looking =
            projection * glm::lookAtRH(glm::vec3(0.5f, 0.5f, -3.0f), glm::vec3(0.5f, 0.5f, 0.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 lookingAway =
            projection * glm::lookAtRH(glm::vec3(0.5f, 0.5f, -3.0f), glm::vec3(0.5f, 0.5f, -6.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));

        vkm::VkmFrameData seeing;
        vkm::vkmExtractFrustumPlanes(looking, seeing._frustumPlanes);
        vkm::VkmFrameData blind;
        vkm::vkmExtractFrustumPlanes(lookingAway, blind._frustumPlanes);

        std::vector<vkm::VkmResourceHandle> referenced;
        scene.collectReferencedResources(&referenced);

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* updateSubGraph = renderGraph.beginTransferSubGraph("CullViewsUpdate");
        for (vkm::VkmResourceHandle handle : referenced) { updateSubGraph->addReferencedResource(handle); }
        // Both views published in one subgraph, which is the arrangement a GI frame uses: it means
        // neither cull ever writes frame data the other has already read.
        updateSubGraph->setTransferCallback([&scene, seeing, blind](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordUpdate(commandBuffer, /*frameIndex=*/0, seeing, /*viewIndex=*/0);
            scene.recordUpdate(commandBuffer, /*frameIndex=*/0, blind, /*viewIndex=*/1);
        });

        auto* cullSubGraph = renderGraph.beginComputeSubGraph("CullViewsCull");
        for (vkm::VkmResourceHandle handle : referenced) { cullSubGraph->addReferencedResource(handle); }
        cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordCull(commandBuffer, /*viewIndex=*/0);
            scene.recordCull(commandBuffer, /*viewIndex=*/1);
        });

        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        CHECK(readVisibleCountForView(driver, scene, /*viewIndex=*/0) == 1);
        CHECK(readVisibleCountForView(driver, scene, /*viewIndex=*/1) == 0);

        scene.destroy(driver);
    }
} // namespace vkmtest

#endif // TEST_SCENE_CULL_VIEWS_SHARED_HPP
