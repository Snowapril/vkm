// Copyright (c) 2025 Snowapril
//
// Phase 7's gate: the 1-spp indirect pass converges to the Phase 6 reference when accumulated.
//
// This is what proves the sampling and the BRDF are right *before* reservoirs exist, when a bias
// is still attributable. The two estimators differ in exactly one thing -- where the primary hit
// comes from -- and everything downstream of it is the shared `vkmTracePath`. So what the
// comparison actually pins is the seam between a rasterizer and a ray query: clip-space
// convention, camera-distance reconstruction, octahedral normal packing, the winding a wall was
// authored with, and the offset a secondary ray leaves along. Every one of those is a silent
// failure if it is wrong, and every one of them moves this number.
//
// The scene is `gltf_cornell.gltf`: an open-top, open-front Cornell-style box with red and green
// side walls, lit by the uniform environment through its missing ceiling. Open-top rather than
// emissive-ceiling on purpose -- the G-buffer carries no emissive channel, so a deferred pass
// cannot reproduce a camera-visible emitter, and lighting the box from the environment removes the
// question rather than papering over it.
#pragma once

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/indirect_pass.h>
#include <vkm/renderer/path_tracer.h>
#include <vkm/renderer/restir.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vkmtest
{
    namespace detail
    {
        // Square, so the comparison is not dominated by whichever axis the box is longer on, and
        // small: both estimators run at one ray per pixel per sample, and the Vulkan half of this
        // runs on a software rasterizer in CI.
        inline constexpr uint32_t kCornellSize = 48;
        // Enough that the estimator's own noise is well under the threshold below. Both sides get
        // the same count, so neither is advantaged.
        inline constexpr uint32_t kCornellSamples = 1536;
        // Scatters the indirect pass takes from the G-buffer surface. The reference needs one more
        // to compute the same quantity: its first bounce is the primary ray this pass does not
        // cast.
        inline constexpr uint32_t kIndirectBounces = 3;
        inline constexpr float kEnvironmentRadiance = 1.0f;

        inline std::vector<float> readAccumulationBuffer(vkm::VkmDriverBase* driver,
                                                         vkm::VkmResourceHandle source,
                                                         uint32_t width, uint32_t height)
        {
            const uint64_t byteSize = static_cast<uint64_t>(width) * height * 4 * sizeof(float);

            vkm::VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
            stagingInfo._size = byteSize;
            stagingInfo._debugName = "IndirectComparisonReadback";
            vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            REQUIRE(staging != nullptr);

            const vkm::VkmResourceHandle destination = staging->getHandle();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginTransferSubGraph("IndirectComparisonReadback");
            subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->copyBuffer(source, destination, 0, 0, byteSize);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            std::vector<float> rgba(static_cast<size_t>(width) * height * 4);
            staging->invalidate(0, byteSize);
            std::memcpy(rgba.data(), staging->map(), byteSize);
            // The graph's completion wait is a timeline wait; see TestAccelerationStructureShared.
            driver->waitIdle();
            driver->getRenderResourcePool()->releaseResource(destination);
            return rgba;
        }
    } // namespace detail

    inline void runIndirectConvergenceTest(vkm::VkmDriverBase* driver)
    {
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
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_cornell.gltf",
                                             &model, &error, importOptions),
                        error);

        vkm::VkmPipelineStateManager manager(driver);
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &error),
                        error);

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        REQUIRE_MESSAGE(scene.build(driver, &manager, &error), error);
        REQUIRE(scene.getObjects().size() == 4); // floor, back, and the two coloured side walls
        REQUIRE_MESSAGE(scene.buildAccelerationStructures(driver, &error), error);

        // Inside the box near its open front, looking at the back wall. The ceiling is missing, so
        // there is nothing above to be seen directly -- rays that leave through the opening return
        // the environment, which is what lights the box.
        const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 0.85f), glm::vec3(0.0f, 0.0f, -1.0f),
                                             glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.05f, 20.0f);
        const glm::mat4 viewProjection = projection * view;

        vkm::VkmFrameConstants frameConstants{};
        frameConstants._view = view;
        frameConstants._projection = projection;
        frameConstants._viewProjection = viewProjection;
        frameConstants._inverseViewProjection = glm::inverse(viewProjection);
        frameConstants._prevViewProjection = viewProjection;
        frameConstants._cameraPositionWorld = glm::vec4(0.0f, 0.0f, 0.85f, 1.0f);
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);

        std::vector<vkm::VkmResourceHandle> referenced;
        scene.collectReferencedResources(&referenced);
        referenced.push_back(scene.getTopLevelAccelerationStructure());

        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, glm::uvec2(detail::kCornellSize, detail::kCornellSize)));

        REQUIRE_MESSAGE(vkm::vkmLoadRayTracingPipelineStates(&manager, &error), error);

        vkm::VkmPathTracer reference;
        REQUIRE_MESSAGE(reference.initialize(driver, &manager, detail::kCornellSize,
                                             detail::kCornellSize, &error),
                        error);
        vkm::VkmIndirectPass indirect;
        REQUIRE_MESSAGE(indirect.initialize(driver, &manager, gbuffer, detail::kCornellSize,
                                            detail::kCornellSize, &error),
                        error);
        vkm::VkmRestirPass restir;
        REQUIRE_MESSAGE(restir.initialize(driver, &manager, gbuffer, detail::kCornellSize,
                                          detail::kCornellSize, &error),
                        error);
        vkm::VkmRestirPass spatial;
        REQUIRE_MESSAGE(spatial.initialize(driver, &manager, gbuffer, detail::kCornellSize,
                                           detail::kCornellSize, &error),
                        error);

        const vkm::VkmPathTraceOptions referenceOptions{
            /*_maxBounces=*/detail::kIndirectBounces + 1,
            // Pixel centres, so both estimators start from the same primary point; otherwise every
            // silhouette pixel differs for a reason that has nothing to do with the estimator.
            /*_jitterPrimaryRay=*/false,
            glm::vec3(detail::kEnvironmentRadiance)
        };
        const vkm::VkmIndirectOptions indirectOptions{
            detail::kIndirectBounces,
            glm::vec3(detail::kEnvironmentRadiance)
        };
        /*
        * Phase 8.1/8.3: the same estimator again, but routed through a reservoir. Slice 1 rather
        * than slice 0 on purpose -- with only one slice exercised, a pass that ignored the slice
        * index entirely would still pass, and the index is the mechanism 8.4 will read one slice
        * and write another through.
        */
        vkm::VkmRestirOptions restirOptions{};
        restirOptions._maxBounces = detail::kIndirectBounces;
        restirOptions._environmentRadiance = glm::vec3(detail::kEnvironmentRadiance);
        // Slice 1 in, slice 0 out -- deliberately not the default pair. With only slice 0
        // exercised, a pass that ignored the slice index entirely would still pass, and the index
        // is the mechanism spatial reuse reads one slice and writes another through.
        restirOptions._inputSlice = 1;
        restirOptions._outputSlice = 0;

        // Phase 8.4 runs alongside 8.3 rather than replacing it: the whole question this sub-step
        // is measured by is whether turning resampling on moves the mean, which needs both.
        vkm::VkmRestirOptions spatialOptions = restirOptions;
        spatialOptions._spatialResampling = true;

        // One G-buffer fill: the camera and the scene are static, so the indirect pass reads the
        // same surfaces every sample and only its rays change.
        {
            const vkm::VkmFrameBufferDescriptor fbDesc = gbuffer.makeFrameBufferDescriptor();

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("CornellSceneUpdate");
            for (vkm::VkmResourceHandle handle : referenced) { updateSubGraph->addReferencedResource(handle); }
            updateSubGraph->setTransferCallback([&scene, frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });

            auto* cullSubGraph = renderGraph.beginComputeSubGraph("CornellSceneCull");
            for (vkm::VkmResourceHandle handle : referenced) { cullSubGraph->addReferencedResource(handle); }
            cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordCull(commandBuffer, /*viewIndex=*/0);
            });

            auto* gbufferSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc, "CornellGBuffer");
            for (vkm::VkmResourceHandle handle : referenced) { gbufferSubGraph->addReferencedResource(handle); }
            gbufferSubGraph->setRenderCallback([&scene, &manager](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordDrawBatches(commandBuffer, [&manager](const vkm::VkmScene::DrawBatch& batch) {
                    return manager.getPipelineState(
                        std::string("gbuffer_pso[") + vkm::vkmVertexLayoutPresetName(batch._layout) + "]",
                        vkm::VkmPipelineStateOrigin::Engine);
                });
            });

            // Everything the estimators sample has to leave its attachment layout first. Without
            // this the G-buffer is read while still in COLOR_ATTACHMENT_OPTIMAL, which Metal does
            // not care about and Vulkan reports as VUID-vkCmdDraw-None-09600 -- and the reads come
            // back as nothing, so the coverage REQUIRE below is what actually fails. Same shape as
            // the gi sample's GiGBufferToShaderRead subgraph.
            auto* barrierSubGraph = renderGraph.beginComputeSubGraph("CornellGBufferToShaderRead");
            for (vkm::VkmResourceHandle handle : referenced) { barrierSubGraph->addReferencedResource(handle); }
            barrierSubGraph->setComputeCallback([&gbuffer](vkm::VkmCommandBufferBase* commandBuffer) {
                for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
                {
                    commandBuffer->barrierTextureForShaderRead(
                        gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)));
                }
            });

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }

        // Both estimators, sample for sample, in one graph per sample so neither can be advantaged
        // by a different number of submissions.
        for (uint32_t sample = 0; sample < detail::kCornellSamples; ++sample)
        {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginComputeSubGraph("CornellEstimators");
            for (vkm::VkmResourceHandle handle : referenced) { subGraph->addReferencedResource(handle); }
            subGraph->addReferencedResource(reference.getAccumulationBuffer());
            subGraph->addReferencedResource(indirect.getAccumulationBuffer());
            subGraph->addReferencedResource(restir.getAccumulationBuffer());
            subGraph->addReferencedResource(restir.getReservoirBuffer());
            subGraph->addReferencedResource(spatial.getAccumulationBuffer());
            subGraph->addReferencedResource(spatial.getReservoirBuffer());
            subGraph->setComputeCallback([&](vkm::VkmCommandBufferBase* commandBuffer) {
                reference.recordAccumulate(commandBuffer, referenceOptions);
                indirect.recordAccumulate(commandBuffer, indirectOptions);
                restir.recordAccumulate(commandBuffer, restirOptions);
                spatial.recordAccumulate(commandBuffer, spatialOptions);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }
        CHECK(reference.getSampleCount() == detail::kCornellSamples);
        CHECK(indirect.getSampleCount() == detail::kCornellSamples);
        CHECK(restir.getSampleCount() == detail::kCornellSamples);
        CHECK(spatial.getSampleCount() == detail::kCornellSamples);

        const std::vector<float> referenceImage = detail::readAccumulationBuffer(
            driver, reference.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const std::vector<float> indirectImage = detail::readAccumulationBuffer(
            driver, indirect.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const std::vector<float> restirImage = detail::readAccumulationBuffer(
            driver, restir.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const std::vector<float> spatialImage = detail::readAccumulationBuffer(
            driver, spatial.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const uint32_t pixelCount = detail::kCornellSize * detail::kCornellSize;

        /*
        * Both sides have to be shown non-empty BEFORE the comparison, because
        * vkmComputeImageMse returns 0 when nothing is comparable -- a reference that never ran
        * scores a perfect match. That is not hypothetical: it is exactly what this test reported
        * while VkmIndirectPass::initialize was still loading the PSO directory a second time and
        * destroying the pipeline VkmPathTracer held.
        */
        const auto summarize = [&](const std::vector<float>& image, uint32_t* outCovered) {
            double brightness = 0.0;
            uint32_t covered = 0;
            for (uint32_t pixel = 0; pixel < pixelCount; ++pixel)
            {
                const float samples = image[pixel * 4 + 3];
                if (samples > 0.0f)
                {
                    brightness += image[pixel * 4 + 0] / samples;
                    ++covered;
                }
            }
            *outCovered = covered;
            return covered == 0 ? 0.0 : brightness / covered;
        };

        uint32_t referenceCovered = 0;
        const double referenceBrightness = summarize(referenceImage, &referenceCovered);
        uint32_t covered = 0;
        const double indirectBrightness = summarize(indirectImage, &covered);

        /*
         * Where the covered pixels sit, not just how many. This test is the first thing that ever
         * ran the engine's G-buffer raster path on Vulkan, and it fails there on coverage alone --
         * so the question is whether the rasteriser drew nothing or drew the box the other way up.
         * The Cornell box is open at the top, so a vertically mirrored raster puts the opening
         * where the floor should be and covers the opposite half. stderr, because a failing REQUIRE
         * below aborts before doctest prints anything else.
         */
        uint32_t coveredTopHalf = 0;
        for (uint32_t pixel = 0; pixel < pixelCount / 2; ++pixel)
        {
            if (indirectImage[pixel * 4 + 3] > 0.0f) { ++coveredTopHalf; }
        }
        std::fprintf(stderr,
                     "[indirect] pixels %u, reference covered %u, indirect covered %u "
                     "(first half %u, second half %u)\n",
                     pixelCount, referenceCovered, covered, coveredTopHalf, covered - coveredTopHalf);
        std::fflush(stderr);

        // The reference traces every pixel; the deferred pass only those the G-buffer covered.
        REQUIRE(referenceCovered == pixelCount);
        REQUIRE(covered > pixelCount / 2);
        // Lit, so "they agree" is not a statement about two black images.
        CHECK(referenceBrightness > 0.05);
        CHECK(indirectBrightness > 0.05);

        const float mse = vkm::vkmComputeImageMse(referenceImage.data(), indirectImage.data(), pixelCount);
        const float relativeMse =
            vkm::vkmComputeImageRelativeMse(referenceImage.data(), indirectImage.data(), pixelCount);
        MESSAGE("indirect vs reference over " << covered << " covered pixels: MSE " << mse
                                              << ", RelMSE " << relativeMse
                                              << ", mean red ref " << referenceBrightness
                                              << " vs indirect " << indirectBrightness);

        /*
        * Measured, with margin, not guessed. At 1536 samples the two land at MSE 2.4e-4 and
        * RelMSE 1.6e-3 -- the Monte Carlo noise of two independent runs rather than any
        * disagreement, with their mean radiances 0.15% apart. Both streams are seeded
        * deterministically, so that floor is reproducible rather than something a run can be
        * unlucky with.
        *
        * The sample count is chosen to put that floor *below* the smallest systematic error worth
        * catching, since noise falls as 1/N while a bias does not. It was 384 first, and at that
        * count a reference run one bounce short scored 1.1e-3 against a noise floor of 6.2e-4 --
        * too close to separate. At 1536 the same sabotage is roughly 3x the floor and the
        * threshold sits between them. Cheap insurance: the whole test is 0.8 s on Metal.
        *
        * A gross error is not subtle at this scale. The geometric normal pointing away from the
        * camera -- which is what this gate found on its first real run -- scored MSE 0.030, over a
        * hundred times the floor.
        */
        CHECK(mse < 6.0e-4f);
        CHECK(relativeMse < 4.0e-3f);

        /*
        * Phase 8.1/8.3's own gate, and it is much sharper than the one above.
        *
        * With a single candidate, RIS reduces to the estimator it resamples: W = 1/p_source, and
        * `f_s * cos * L * W` is `albedo * L`. The reservoir pass also draws from gi_indirect's
        * random stream on purpose, so the two see the *same* direction at every pixel of every
        * sample. Everything about the two images therefore has to agree except what the reservoir
        * round trip loses -- RGB9E5 radiance, an octahedral snorm16 normal, and a direction
        * recovered from a stored position rather than carried.
        *
        * So this is not "does it converge to the same thing" but "is it the same estimator", and
        * it is checked two orders of magnitude tighter than the convergence gate above. A wrong
        * W, a dropped pi, a mis-packed exponent or a slice index nobody read all move it well
        * past this.
        */
        uint32_t restirCovered = 0;
        const double restirBrightness = summarize(restirImage, &restirCovered);
        CHECK(restirCovered == covered);
        CHECK(restirBrightness > 0.05);

        const float packingMse =
            vkm::vkmComputeImageMse(indirectImage.data(), restirImage.data(), pixelCount);
        const float packingRelativeMse =
            vkm::vkmComputeImageRelativeMse(indirectImage.data(), restirImage.data(), pixelCount);
        MESSAGE("reservoir vs 1-spp: MSE " << packingMse << ", RelMSE " << packingRelativeMse
                                           << ", mean red " << restirBrightness);
        // Measured at 9.7e-7 / 1.8e-5, with the mean radiances 0.09% apart -- the whole of which
        // is RGB9E5's nine mantissa bits. Three times that, so quantization noise cannot trip it,
        // and still two orders of magnitude under the convergence gate above. An RGB9E5 exponent
        // bias off by one -- which is what this caught on its first run -- scored 5.5e-4, and a
        // slice index nobody read leaves the resolve reading zeros.
        CHECK(packingMse < 3.0e-6f);
        CHECK(packingRelativeMse < 6.0e-5f);

        /*
        * Phase 8.4 is dispatched but NOT yet asserted against ground truth, and saying so here is
        * the point: its per-neighbour visibility ray makes the image 13.8% bright, and until that
        * is understood the pass is not something to hold the engine to (see TODO.md and the header
        * of gi_reservoir_spatial.hlsl for what was measured).
        *
        * What is checked is what is true: the pass runs, covers the same pixels the others do, and
        * produces a lit image under a validation layer. That keeps it compiled, dispatched and
        * exercised rather than rotting, without pretending the estimator is verified.
        */
        uint32_t spatialCovered = 0;
        const double spatialBrightness = summarize(spatialImage, &spatialCovered);
        const float spatialMse =
            vkm::vkmComputeImageMse(referenceImage.data(), spatialImage.data(), pixelCount);
        const float spatialRelativeMse =
            vkm::vkmComputeImageRelativeMse(referenceImage.data(), spatialImage.data(), pixelCount);
        // Printed rather than asserted, and printed with the ratio because that is the number a
        // reader wants: 1.0 means resampling did not move the mean. On this machine's Metal it
        // reads 1.138, and whether it reads the same on a different driver is what says whether
        // the fault is in the estimator or in one backend's code generation.
        MESSAGE("spatial (UNVERIFIED) vs reference: MSE " << spatialMse << ", RelMSE "
                                                          << spatialRelativeMse << ", mean red "
                                                          << spatialBrightness << " (1-spp "
                                                          << indirectBrightness << ", ratio "
                                                          << (spatialBrightness / indirectBrightness)
                                                          << ")");
        CHECK(spatialCovered == covered);
        CHECK(spatialBrightness > 0.05);

        // scene.destroy releases through the deferred reclaimer, whose worker frees on another
        // thread while the next test is already allocating; see TestAccelerationStructureShared.
        driver->waitIdle();
        spatial.destroy(driver);
        restir.destroy(driver);
        indirect.destroy(driver);
        reference.destroy(driver);
        gbuffer.destroy();
        scene.destroy(driver);
        // scene.destroy defers to the reclaimer's worker thread, which would otherwise still be
        // destroying GPU objects while the next test case allocates. Finish it here instead.
        driver->getDeferredReclaimer()->flushBlocking();
    }
} // namespace vkmtest
