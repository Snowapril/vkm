// Copyright (c) 2025 Snowapril
//
// Phase 5's gate: a compute shader ray-casts a loaded glTF scene and writes hit/miss plus `t`
// matching a CPU reference for known rays, on Vulkan and Metal.
//
// This is what makes everything under it observable for the first time. Until a ray traverses the
// structure, "the build accepted it" is the whole of what any acceleration structure test can say
// -- a wrong vertex offset, a wrong index base, a transposed instance transform and a correct
// build are indistinguishable. Six rays against a triangle at a known place distinguish them all.
//
// Written against VkmDriverBase so Metal and Vulkan run the same assertions; a backend without
// VkmDriverCapabilityFlags::RayTracing skips.
#pragma once

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vkmtest
{
    namespace detail
    {
        // Mirrors kRayOrigins in ray_query.hlsl. Kept here rather than uploaded so the shader has
        // exactly one input -- the acceleration structure -- and a wrong result cannot be blamed
        // on a buffer that failed to bind.
        struct RayQueryCase
        {
            float x;
            float y;
            bool expectHit;
            const char* what;
        };
        inline constexpr RayQueryCase kRayQueryCases[6] = {
            { 0.10f,  0.10f, true,  "inside" },
            { 0.25f,  0.25f, true,  "inside" },
            { 0.05f,  0.90f, true,  "inside, near the y edge" },
            { 0.60f,  0.60f, false, "outside: past the hypotenuse x + y = 1" },
            { 2.00f,  2.00f, false, "outside the bounding box entirely" },
            { -0.10f, 0.10f, false, "outside: negative x" },
        };
        inline constexpr uint32_t kRayCount = 6;
        // Two words per ray: [hit ? 1 : 0, asuint(t)].
        inline constexpr uint32_t kRayResultWordCount = kRayCount * 2;
        // Where the traced object is placed, mirroring kInstanceOrigin in ray_query.hlsl. Not the
        // origin, so a build that dropped or transposed the instance transform misses everything;
        // z = -1 against rays starting at z = +2 is what fixes the expected distance at 3.
        inline const glm::mat4 kTracedObjectTransform =
            glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, -1.0f));
        inline constexpr float kExpectedHitDistance = 3.0f;
        // Far enough that no ray can reach it, and its own geometry spans only a few units.
        inline const glm::mat4 kDistractorTransform =
            glm::translate(glm::mat4(1.0f), glm::vec3(1000.0f, 1000.0f, 1000.0f));
    } // namespace detail

    inline void runRayQueryTest(vkm::VkmDriverBase* driver)
    {
        /*
         * Phase markers on stderr rather than doctest's INFO. INFO is printed with a test's
         * context when an assertion FAILS; a SIGSEGV prints none of it, which is why the markers
         * the scene structure test has carried since its first CI run have never appeared in a
         * crash log. stderr is unbuffered, so a marker written before the faulting statement
         * survives it.
         *
         * They cover the whole body, not just teardown: the teardown-only round proved the
         * lavapipe segfault happens BEFORE the first teardown statement, which is not where the
         * assertion count alone suggested.
         */
        const auto mark = [](const char* what) { std::fprintf(stderr, "[rayquery] %s\n", what); std::fflush(stderr); };

        mark("entry");
        REQUIRE(driver != nullptr);
        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            MESSAGE("Skipping: this backend reports no RayTracing capability.");
            return;
        }

        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        std::string error;
        // The distractor goes in FIRST so the triangle's mesh lands at non-zero vertex and index
        // offsets inside the geometry pool. That is what makes the offsets observable: with them
        // zeroed the triangle's structure is built over the distractor's data instead, and these
        // rays stop agreeing with the reference. A second copy of the same model would not do --
        // identical bytes at a different offset produce an identical structure.
        mark("import distractor");
        vkm::VkmSceneModel distractor;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_two_rooms.gltf",
                                             &distractor, &error, importOptions),
                        error);
        mark("import triangle");
        vkm::VkmSceneModel model;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf",
                                             &model, &error, importOptions),
                        error);

        mark("load engine pipeline states");
        vkm::VkmPipelineStateManager engineManager(driver);
        REQUIRE_MESSAGE(engineManager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                      TEST_ENGINE_SHADER_CACHE_DIR,
                                                                      vkm::VkmPipelineStateOrigin::Engine, &error),
                        error);

        mark("scene.addModel x2");
        vkm::VkmScene scene;
        REQUIRE(scene.addModel(distractor, &error));
        const uint32_t tracedMeshEntry = static_cast<uint32_t>(distractor._meshes.size());
        REQUIRE(scene.addModel(model, &error));
        mark("scene.build");
        REQUIRE_MESSAGE(scene.build(driver, &engineManager, &error), error);

        // build() sorts the objects into draw batches, so the triangle is not simply the last one.
        uint32_t tracedObject = vkm::INVALID_VALUE32;
        for (uint32_t object = 0; object < scene.getObjects().size(); ++object)
        {
            const bool isTraced = scene.getObjects()[object]._meshEntryIndex == tracedMeshEntry;
            scene.setObjectTransform(object, isTraced ? detail::kTracedObjectTransform
                                                      : detail::kDistractorTransform);
            if (isTraced)
            {
                tracedObject = object;
            }
        }
        REQUIRE(tracedObject != vkm::INVALID_VALUE32);

        mark("scene.buildAccelerationStructures");
        REQUIRE_MESSAGE(scene.buildAccelerationStructures(driver, &error), error);

        mark("load ray query pipeline states");
        vkm::VkmPipelineStateManager rayQueryManager(driver);
        REQUIRE_MESSAGE(rayQueryManager.loadPipelineStatesFromDirectory(TEST_RAY_QUERY_PSO_DIR,
                                                                        TEST_RAY_QUERY_SHADER_CACHE_DIR,
                                                                        vkm::VkmPipelineStateOrigin::User, &error),
                        error);
        vkm::VkmPipelineStateBase* pso =
            rayQueryManager.getPipelineState("ray_query_pso[default]", vkm::VkmPipelineStateOrigin::User);
        REQUIRE(pso != nullptr);

        const uint64_t resultByteSize = sizeof(uint32_t) * detail::kRayResultWordCount;
        vkm::VkmBufferInfo resultInfo{};
        resultInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite | vkm::VkmResourceCreateInfo::AllowTransferSrc;
        resultInfo._size = resultByteSize;
        resultInfo._debugName = "RayQueryResult";
        mark("create result buffer");
        vkm::VkmBuffer* result = driver->newBuffer(resultInfo);
        REQUIRE(result != nullptr);

        mark("create pass resource table");
        vkm::VkmResourceTableBase* passTable = driver->newResourceTable(
            pso, vkm::VkmResourceSetKind::PerPass, {{ 0, result->getHandle() }}, &error);
        REQUIRE_MESSAGE(passTable != nullptr, error);

        {
            mark("dispatch");
            const vkm::VkmResourceHandle tlas = scene.getTopLevelAccelerationStructure();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginComputeSubGraph("RayQueryDispatch");
            subGraph->addReferencedResource(tlas);
            subGraph->addReferencedResource(result->getHandle());
            subGraph->setComputeCallback([pso, passTable](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pso);
                commandBuffer->bindResourceTable(passTable);
                commandBuffer->dispatch(1);
                commandBuffer->unbindPipeline();
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }

        mark("readback");
        std::vector<uint32_t> words(detail::kRayResultWordCount, 0);
        {
            vkm::VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
            stagingInfo._size = resultByteSize;
            stagingInfo._debugName = "RayQueryReadback";
            vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            REQUIRE(staging != nullptr);

            const vkm::VkmResourceHandle source = result->getHandle();
            const vkm::VkmResourceHandle destination = staging->getHandle();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginTransferSubGraph("RayQueryReadback");
            subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->copyBuffer(source, destination, 0, 0, resultByteSize);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            staging->invalidate(0, resultByteSize);
            std::memcpy(words.data(), staging->map(), resultByteSize);
            // Same reason as the teardown below: the render graph's own completion wait is a
            // timeline wait, which is not the API considering the copy's resources destroyable.
            driver->waitIdle();
            driver->getRenderResourcePool()->releaseResource(destination);
        }

        for (uint32_t ray = 0; ray < detail::kRayCount; ++ray)
        {
            const detail::RayQueryCase& expected = detail::kRayQueryCases[ray];
            const bool hit = words[ray * 2 + 0] != 0;
            float distance = 0.0f;
            std::memcpy(&distance, &words[ray * 2 + 1], sizeof(float));

            CHECK_MESSAGE(hit == expected.expectHit,
                          "ray " << ray << " at (" << expected.x << ", " << expected.y << ") -- "
                                 << expected.what);
            if (expected.expectHit && hit)
            {
                // The triangle's plane is at z = -1 and every ray starts at z = +2, so a correct
                // hit is at exactly 3. Something merely in the way would still report *a* hit; the
                // distance is what pins where the geometry actually ended up.
                CHECK(distance == doctest::Approx(detail::kExpectedHitDistance).epsilon(1e-4));
            }
            if (!expected.expectHit)
            {
                CHECK(distance == 0.0f);
            }
        }

        // Wait for the device before tearing any of this down. renderGraph.ensureCompleted() waits
        // on the graph's timeline, which proves the GPU reached the value but does not retire the
        // submissions as far as the API is concerned -- and scene.destroy releases through the
        // deferred reclaimer, whose worker then frees on another thread while the next test is
        // already allocating. See TestAccelerationStructureShared.hpp.
        /*
         * Phase markers on stderr rather than doctest's INFO. INFO is printed with a test's
         * context when an assertion FAILS; a SIGSEGV prints none of it, which is why the ones the
         * scene structure test carries have never appeared in a crash log. stderr is unbuffered,
         * so a marker written before the faulting statement survives it. This teardown has now
         * segfaulted on lavapipe three times with every assertion passing, inside a stripped
         * driver library that resolves to one bare address.
         */
        mark("teardown: waitIdle");
        driver->waitIdle();
        mark("teardown: passTable->destroy");
        passTable->destroy();
        delete passTable;
        mark("teardown: release result buffer");
        driver->getRenderResourcePool()->releaseResource(result->getHandle());
        mark("teardown: scene.destroy");
        scene.destroy(driver);
        // scene.destroy defers to the reclaimer's worker thread, which would otherwise still be
        // destroying GPU objects while the next test case allocates. Finish it here instead.
        mark("teardown: reclaimer flushBlocking");
        driver->getDeferredReclaimer()->flushBlocking();
        mark("teardown: leaving the test body (pipeline managers and scene destruct here)");
    }
} // namespace vkmtest
