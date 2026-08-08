// Copyright (c) 2025 Snowapril
//
// VkmScene builds its acceleration structures out of the geometry pool it already owns, so a
// traced scene duplicates no vertex data. Written against VkmDriverBase so Metal and Vulkan run
// the same assertions; a backend without VkmDriverCapabilityFlags::RayTracing skips.
#pragma once

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

namespace vkmtest
{
    inline void runSceneAccelerationStructureTest(vkm::VkmDriverBase* driver)
    {
        /*
         * Phase markers. doctest prints a test's logged context when it fails OR crashes, and
         * costs nothing when it does neither -- which is what makes them worth having in a test
         * that only ever executes on a driver this machine does not have. A segmentation fault
         * here otherwise reports a line number and a test name and nothing else, and the first CI
         * run of this test did exactly that.
         */
        INFO("phase: entry");
        REQUIRE(driver != nullptr);
        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            MESSAGE("Skipping: this backend reports no RayTracing capability.");
            return;
        }

        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf",
                                             &model, &error, importOptions),
                        error);

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);

        INFO("phase: scene setup");
        vkm::VkmScene scene;
        // Twice, so the pool holds two meshes and the second one's ranges start at non-zero
        // offsets -- one mesh would leave every offset at 0 and exercise nothing. **This does not
        // verify the offsets**: zeroing both was measured to leave the test passing, because a
        // wrong-but-in-range address builds a structure over the wrong triangles and nothing here
        // traverses it. Only Phase 5's ray-query gate can see that; see TODO.md.
        REQUIRE(scene.addModel(model, &error));
        REQUIRE(scene.addModel(model, &error));
        REQUIRE_MESSAGE(scene.build(driver, &manager, &error), error);
        REQUIRE(scene.getObjects().size() == 2);

        // Nothing is built until asked for: a scene that is only rasterized pays nothing.
        CHECK(scene.getTopLevelAccelerationStructure() == vkm::VKM_INVALID_RESOURCE_HANDLE);

        INFO("phase: buildAccelerationStructures");
        REQUIRE_MESSAGE(scene.buildAccelerationStructures(driver, &error), error);
        INFO("phase: structures built and published");

        const vkm::VkmResourceHandle tlasHandle = scene.getTopLevelAccelerationStructure();
        REQUIRE(tlasHandle != vkm::VKM_INVALID_RESOURCE_HANDLE);
        vkm::VkmAccelerationStructure* tlas =
            driver->getRenderResourcePool()->getResource<vkm::VkmAccelerationStructure>(tlasHandle);
        REQUIRE(tlas != nullptr);
        // Sized against real geometry. This is what fails when a mesh range reaches the build
        // empty: zeroing the index count makes the bottom-level build refuse outright, verified by
        // sabotage.
        CHECK(tlas->getAllocatedSize() > 0);
        CHECK(tlas->getType() == vkm::VkmAccelerationStructureType::TopLevel);

        SUBCASE("building twice is refused rather than leaking the first set")
        {
            CHECK_FALSE(scene.buildAccelerationStructures(driver, &error));
            CHECK(scene.getTopLevelAccelerationStructure() == tlasHandle);
        }

        SUBCASE("an object that moves is republished and the structure rebuilt")
        {
            // The falling-body case: only the instance transform changes, and only for one of the
            // two objects. Its bottom-level structure is never touched.
            scene.setObjectTransform(0, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -5.0f, 0.0f)));

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginComputeSubGraph("SceneAccelerationStructureRebuild");
            subGraph->addReferencedResource(tlasHandle, vkm::VkmResourceAccess::AccelerationStructureBuildWrite);
            subGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordAccelerationStructureUpdate(commandBuffer);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            // The rebuild ran on the queue; the structure it wrote into is the same one, still
            // sized as before. Under a validation layer this whole subcase is the assertion: a
            // rebuild without a scratch buffer, or over more instances than the structure was
            // sized for, is reported there rather than in a return value.
            CHECK(tlas->getAllocatedSize() > 0);
        }

        INFO("phase: teardown");
        // scene.destroy releases through the deferred reclaimer, whose worker frees on its own
        // thread as soon as the recorded usages report complete. That is not the same as the API
        // considering the structures destroyable -- see TestAccelerationStructureShared.hpp -- so
        // wait for the device first, as any caller tearing a scene down mid-run would have to.
        driver->waitIdle();
        scene.destroy(driver);
        // scene.destroy defers to the reclaimer's worker thread, which would otherwise still be
        // destroying GPU objects while the next test case allocates. Finish it here instead.
        driver->getDeferredReclaimer()->flushBlocking();
        CHECK(scene.getTopLevelAccelerationStructure() == vkm::VKM_INVALID_RESOURCE_HANDLE);
    }
} // namespace vkmtest
