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
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/indirect_pass.h>
#include <vkm/renderer/path_tracer.h>
#include <vkm/renderer/restir.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

        // IEEE half to float, for reading an R16G16B16A16_SFLOAT target back raw --
        // driver->readbackTexture() converts to 8-bit and would clamp HDR values.
        inline float halfToFloat(uint16_t half)
        {
            const uint32_t sign = (half >> 15) & 1u;
            const uint32_t exponent = (half >> 10) & 0x1Fu;
            const uint32_t mantissa = half & 0x3FFu;
            float value;
            if (exponent == 0)
            {
                value = static_cast<float>(mantissa) * 5.9604644775390625e-8f; // 2^-24
            }
            else if (exponent == 31)
            {
                value = std::numeric_limits<float>::infinity();
            }
            else
            {
                value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                                   static_cast<int>(exponent) - 15);
            }
            return sign != 0 ? -value : value;
        }

        // Raw texture bytes via copyTextureToBuffer: mip 0, tightly packed, no format conversion.
        inline std::vector<uint8_t> readTextureBytes(vkm::VkmDriverBase* driver,
                                                     vkm::VkmResourceHandle source,
                                                     uint64_t byteSize)
        {
            vkm::VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
            stagingInfo._size = byteSize;
            stagingInfo._debugName = "TextureRawReadback";
            vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            REQUIRE(staging != nullptr);

            const vkm::VkmResourceHandle destination = staging->getHandle();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginTransferSubGraph("TextureRawReadback");
            subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->copyTextureToBuffer(source, destination);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            std::vector<uint8_t> bytes(static_cast<size_t>(byteSize));
            staging->invalidate(0, byteSize);
            std::memcpy(bytes.data(), staging->map(), byteSize);
            driver->waitIdle();
            driver->getRenderResourcePool()->releaseResource(destination);
            return bytes;
        }

        // The same staging round trip, shaped for the reservoir buffer's raw words.
        inline std::vector<uint32_t> readBufferWords(vkm::VkmDriverBase* driver,
                                                     vkm::VkmResourceHandle source,
                                                     uint64_t byteSize)
        {
            vkm::VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
            stagingInfo._size = byteSize;
            stagingInfo._debugName = "ReservoirReadback";
            vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            REQUIRE(staging != nullptr);

            const vkm::VkmResourceHandle destination = staging->getHandle();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginTransferSubGraph("ReservoirReadback");
            subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->copyBuffer(source, destination, 0, 0, byteSize);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            std::vector<uint32_t> words(static_cast<size_t>(byteSize / sizeof(uint32_t)));
            staging->invalidate(0, byteSize);
            std::memcpy(words.data(), staging->map(), byteSize);
            driver->waitIdle();
            driver->getRenderResourcePool()->releaseResource(destination);
            return words;
        }
    } // namespace detail

    inline void runIndirectConvergenceTest(vkm::VkmDriverBase* driver, float mseThreshold = 6.0e-4f)
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
        // Static camera: the previous eye is this eye. Leaving it at the origin default rejects
        // every temporal history tap on the depth criterion, which the reservoir readback below
        // catches as M never growing past 1.
        frameConstants._prevCameraPositionWorld = frameConstants._cameraPositionWorld;
        // The lighting pass takes its extent from here (a fragment shader cannot read push
        // constants on Vulkan).
        frameConstants._viewportSize = glm::vec4(
            static_cast<float>(detail::kCornellSize), static_cast<float>(detail::kCornellSize),
            1.0f / detail::kCornellSize, 1.0f / detail::kCornellSize);
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);

        std::vector<vkm::VkmResourceAccessDeclaration> referenced;

        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, glm::uvec2(detail::kCornellSize, detail::kCornellSize)));

        REQUIRE_MESSAGE(vkm::vkmLoadRayTracingPipelineStates(&manager, &error), error);

        // One G-buffer fill per set: the camera and the scene are static, so the passes read the
        // same surfaces every sample and only their rays change. Filling BOTH sets (with a flip
        // between) gives the temporal pass a previous G-buffer with the scene in it -- exactly
        // what a static camera's second frame sees -- rather than an allocated-but-never-rendered
        // set whose zero camera distance reads as a full disocclusion.
        const auto fillGBuffer = [&]() {
            const vkm::VkmFrameBufferDescriptor fbDesc = gbuffer.makeFrameBufferDescriptor();

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("CornellSceneUpdate");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Update, &referenced);
            updateSubGraph->addReferencedResources(referenced);
            updateSubGraph->setTransferCallback([&scene, frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });

            auto* cullSubGraph = renderGraph.beginComputeSubGraph("CornellSceneCull");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Cull, &referenced);
            cullSubGraph->addReferencedResources(referenced);
            cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordCull(commandBuffer, /*viewIndex=*/0);
            });

            auto* gbufferSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc, "CornellGBuffer");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
            gbufferSubGraph->addReferencedResources(referenced);
            gbufferSubGraph->setRenderCallback([&scene, &manager](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordDrawBatches(commandBuffer, [&manager](const vkm::VkmScene::DrawBatch& batch) {
                    return manager.getPipelineState(
                        std::string("gbuffer_pso[") + vkm::vkmVertexLayoutPresetName(batch._layout) + "]",
                        vkm::VkmPipelineStateOrigin::Engine);
                });
            });

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        };
        fillGBuffer();
        gbuffer.advanceFrame();
        fillGBuffer();

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
        vkm::VkmRestirPass temporal;
        REQUIRE_MESSAGE(temporal.initialize(driver, &manager, gbuffer, detail::kCornellSize,
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

        // Phase 8.5, same reasoning. The camera is static, so reprojection is the identity and
        // the history tap is the pixel itself -- which is exactly the sub-plan's "static first".
        vkm::VkmRestirOptions temporalOptions = restirOptions;
        temporalOptions._temporalResampling = true;

        // Both estimators, sample for sample, in one graph per sample so neither can be advantaged
        // by a different number of submissions.
        for (uint32_t sample = 0; sample < detail::kCornellSamples; ++sample)
        {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginComputeSubGraph("CornellEstimators");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
            subGraph->addReferencedResources(referenced);
            subGraph->addReferencedResource(scene.getTopLevelAccelerationStructure(),
                                           vkm::VkmResourceAccess::AccelerationStructureShaderRead);
            // The G-buffer passes in the graphs above left these as attachments; the estimators
            // sample them through set 0, which the render graph only knows about because of this.
            // The previous set too: the temporal pass validates its history taps against it.
            for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
            {
                subGraph->addReferencedResource(gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)),
                                                vkm::VkmResourceAccess::ShaderSampledRead);
                subGraph->addReferencedResource(gbuffer.getPrevTexture(static_cast<vkm::VkmGBuffer::Target>(i)),
                                                vkm::VkmResourceAccess::ShaderSampledRead);
            }
            for (vkm::VkmResourceHandle accumulation : { reference.getAccumulationBuffer(),
                                                         indirect.getAccumulationBuffer(),
                                                         restir.getAccumulationBuffer(),
                                                         restir.getReservoirBuffer(),
                                                         spatial.getAccumulationBuffer(),
                                                         spatial.getReservoirBuffer(),
                                                         temporal.getAccumulationBuffer(),
                                                         temporal.getReservoirBuffer() })
            {
                subGraph->addReferencedResource(accumulation, vkm::VkmResourceAccess::ShaderStorageReadWrite);
            }
            subGraph->setComputeCallback([&](vkm::VkmCommandBufferBase* commandBuffer) {
                reference.recordAccumulate(commandBuffer, referenceOptions);
                indirect.recordAccumulate(commandBuffer, indirectOptions);
                restir.recordAccumulate(commandBuffer, restirOptions);
                spatial.recordAccumulate(commandBuffer, spatialOptions);
                temporal.recordAccumulate(commandBuffer, temporalOptions);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }
        CHECK(reference.getSampleCount() == detail::kCornellSamples);
        CHECK(indirect.getSampleCount() == detail::kCornellSamples);
        CHECK(restir.getSampleCount() == detail::kCornellSamples);
        CHECK(spatial.getSampleCount() == detail::kCornellSamples);
        CHECK(temporal.getSampleCount() == detail::kCornellSamples);

        const std::vector<float> referenceImage = detail::readAccumulationBuffer(
            driver, reference.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const std::vector<float> indirectImage = detail::readAccumulationBuffer(
            driver, indirect.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const std::vector<float> restirImage = detail::readAccumulationBuffer(
            driver, restir.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const std::vector<float> spatialImage = detail::readAccumulationBuffer(
            driver, spatial.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const std::vector<float> temporalImage = detail::readAccumulationBuffer(
            driver, temporal.getAccumulationBuffer(), detail::kCornellSize, detail::kCornellSize);
        const uint32_t pixelCount = detail::kCornellSize * detail::kCornellSize;

        /*
        * Both sides have to be shown non-empty BEFORE the comparison, because vkmComputeImageMse
        * returns 0 when nothing is comparable, so a reference that never ran scores a perfect
        * match.
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
        * `mseThreshold` is per-backend because the floors are, and raising the sample count does
        * not close the gap. Measured: Metal 2.41e-4 at 1536, 1.465e-4 at 6144, 1.318e-4 at 12288;
        * lavapipe 7.945e-4 at 1536 and 6.816e-4 at 12288. Both fall by the same ~1.1e-4 over that
        * range, so both carry the same variance and lavapipe carries about 5.5e-4 of extra
        * *systematic* difference that more samples will never buy down. It is not a difference in
        * the mean -- the deferred pass is 6.3% darker than the reference on Metal and 6.6% darker
        * on lavapipe -- so it is per-pixel structure, most plausibly the silhouette pixels where a
        * software rasteriser's coverage and a ray's intersection disagree, which at 48x48 are a
        * large fraction of the image. TODO.md carries it as unexplained.
        *
        * A gross error is not subtle at this scale. The geometric normal pointing away from the
        * camera -- which is what this gate found on its first real run -- scored MSE 0.030, over a
        * hundred times the floor.
        */
        CHECK(mse < mseThreshold);
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
        * Phase 8.4's gate: turning spatial resampling on must not move the mean. The pass merges
        * four neighbours per pixel under the target function p_hat = luminance * cos * V, with the
        * same p_hat in the merge weights and in the bias-correction denominator -- the asymmetry
        * this gate caught (visibility in the denominator only) read 13.8% bright.
        *
        * The ratio is the number a reader wants: 1.0 means resampling did not move the mean.
        * Measured 0.9977 on Metal at 1536 samples; the tolerance is ~4x that deviation. The MSE
        * thresholds are the same ones the 1-spp estimator is held to: at this sample count both
        * are at their noise floors, and reuse correlation keeps the accumulated RelMSE from
        * *improving* on the baseline -- resampling's win is at low sample counts, which is the
        * live path's regime, not this accumulation's.
        */
        uint32_t spatialCovered = 0;
        const double spatialBrightness = summarize(spatialImage, &spatialCovered);
        const float spatialMse =
            vkm::vkmComputeImageMse(referenceImage.data(), spatialImage.data(), pixelCount);
        const float spatialRelativeMse =
            vkm::vkmComputeImageRelativeMse(referenceImage.data(), spatialImage.data(), pixelCount);
        MESSAGE("spatial vs reference: MSE " << spatialMse << ", RelMSE "
                                             << spatialRelativeMse << ", mean red "
                                             << spatialBrightness << " (1-spp "
                                             << indirectBrightness << ", ratio "
                                             << (spatialBrightness / indirectBrightness)
                                             << ")");
        CHECK(spatialCovered == covered);
        CHECK(spatialBrightness > 0.05);
        CHECK(std::abs(spatialBrightness / indirectBrightness - 1.0) < 0.01);
        CHECK(spatialMse < mseThreshold);
        CHECK(spatialRelativeMse < 4.0e-3f);

        /*
        * Phase 8.5's gate, in two halves.
        *
        * The mean: on a static camera the reprojection is the identity and last frame's p_hat
        * equals this frame's (same normal, same position), so the two-participant merge and its
        * denominator are exact and turning temporal reuse on must not move the mean. The MSE
        * bounds are the baseline's own, as for the spatial pass above: the estimator is heavily
        * correlated across accumulated frames (that is what confidence-weighted reuse IS), so its
        * accumulated floor cannot beat the independent baseline's -- the win is per-frame, in the
        * live path.
        *
        * The structure: the reservoirs themselves are read back and word 6 decoded, because the
        * caps are invisible in the image. M saturating at confidenceCap + 1 (capped history plus
        * the fresh sample) says history was found, merged and capped; a temporal pass reading the
        * wrong slice never grows M past 1, and one ignoring the cap saturates at the field's 255.
        * A maximum age in (0, maxSampleAge] says samples survive reuse AND the stale ones are
        * discarded.
        */
        uint32_t temporalCovered = 0;
        const double temporalBrightness = summarize(temporalImage, &temporalCovered);
        const float temporalMse =
            vkm::vkmComputeImageMse(referenceImage.data(), temporalImage.data(), pixelCount);
        const float temporalRelativeMse =
            vkm::vkmComputeImageRelativeMse(referenceImage.data(), temporalImage.data(), pixelCount);
        MESSAGE("temporal vs reference: MSE " << temporalMse << ", RelMSE "
                                              << temporalRelativeMse << ", mean red "
                                              << temporalBrightness << " (1-spp "
                                              << indirectBrightness << ", ratio "
                                              << (temporalBrightness / indirectBrightness)
                                              << ")");
        CHECK(temporalCovered == covered);
        CHECK(temporalBrightness > 0.05);
        CHECK(std::abs(temporalBrightness / indirectBrightness - 1.0) < 0.01);
        CHECK(temporalMse < mseThreshold);
        CHECK(temporalRelativeMse < 4.0e-3f);

        {
            // The final temporal output lives in the slice written by the LAST recordAccumulate:
            // parity (kCornellSamples - 1) & 1, output slice 1 - parity.
            const uint32_t lastParity = (detail::kCornellSamples - 1) & 1u;
            const uint32_t finalSlice = 1u - lastParity;
            const std::vector<uint32_t> words = detail::readBufferWords(
                driver, temporal.getReservoirBuffer(),
                static_cast<uint64_t>(pixelCount) * vkm::kVkmReservoirSliceCount *
                    vkm::kVkmReservoirByteStride);
            uint32_t maxM = 0;
            uint32_t maxAge = 0;
            for (uint32_t pixel = 0; pixel < pixelCount; ++pixel)
            {
                const uint32_t packed =
                    words[(static_cast<size_t>(finalSlice) * pixelCount + pixel) *
                              vkm::kVkmReservoirWordStride + 6];
                maxM = std::max(maxM, packed & 0xFFu);
                maxAge = std::max(maxAge, (packed >> 8) & 0xFFu);
            }
            MESSAGE("temporal reservoirs: max M " << maxM << ", max age " << maxAge);
            CHECK(maxM == temporalOptions._confidenceCap + 1);
            CHECK(maxAge > 0);
            CHECK(maxAge <= temporalOptions._maxSampleAge);
        }

        /*
        * Phase 8.6: the lighting pass, driven for one frame exactly the way a live renderer
        * drives it -- constants staged in a transfer subgraph, resample in a compute subgraph,
        * the fullscreen draw into an HDR target in a graphics subgraph. The target carries
        * incoming irradiance (no albedo, no 1/pi -- gi_composite's contract), so applying the
        * composite's own factor per pixel must land on the same mean the accumulated compute
        * resolve converged to. That closes the loop between the two consumers of a reservoir:
        * a texture whose mean disagreed with the accumulation would mean the two shading paths
        * read different slices or different equations.
        */
        {
            vkm::VkmTextureInfo targetInfo{};
            targetInfo._flags = static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowColorAttachment) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferSrc));
            targetInfo._extent = glm::uvec3(detail::kCornellSize, detail::kCornellSize, 1);
            targetInfo._numMipLevels = 1;
            targetInfo._numArrayLayers = 1;
            targetInfo._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
            targetInfo._debugName = "RestirLightingTarget";
            vkm::VkmTexture* target = driver->newTexture(targetInfo);
            REQUIRE(target != nullptr);

            vkm::VkmFrameBufferDescriptor fbDesc{};
            fbDesc._width = detail::kCornellSize;
            fbDesc._height = detail::kCornellSize;
            fbDesc._renderPass._colorAttachmentCount = 1;
            fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
            fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
            fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
            fbDesc._colorAttachments[0] = target->getHandle();

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("RestirLightingConstants");
            updateSubGraph->addReferencedResource(temporal.getLightingConstantBuffer(),
                                                  vkm::VkmResourceAccess::TransferWrite);
            updateSubGraph->addReferencedResource(temporal.getLightingStagingBuffer(0),
                                                  vkm::VkmResourceAccess::TransferRead);
            updateSubGraph->setTransferCallback([&](vkm::VkmCommandBufferBase* commandBuffer) {
                temporal.recordUpdateLightingConstants(commandBuffer, /*frameIndex=*/0, temporalOptions,
                                                       vkm::VkmRestirDebugView::Lighting,
                                                       /*misBlend=*/0.0f);
            });

            auto* resampleSubGraph = renderGraph.beginComputeSubGraph("RestirLightingResample");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
            resampleSubGraph->addReferencedResources(referenced);
            resampleSubGraph->addReferencedResource(scene.getTopLevelAccelerationStructure(),
                                                    vkm::VkmResourceAccess::AccelerationStructureShaderRead);
            for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
            {
                resampleSubGraph->addReferencedResource(
                    gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)),
                    vkm::VkmResourceAccess::ShaderSampledRead);
                resampleSubGraph->addReferencedResource(
                    gbuffer.getPrevTexture(static_cast<vkm::VkmGBuffer::Target>(i)),
                    vkm::VkmResourceAccess::ShaderSampledRead);
            }
            resampleSubGraph->addReferencedResource(temporal.getReservoirBuffer(),
                                                    vkm::VkmResourceAccess::ShaderStorageReadWrite);
            resampleSubGraph->setComputeCallback([&](vkm::VkmCommandBufferBase* commandBuffer) {
                temporal.recordResample(commandBuffer, temporalOptions);
            });

            auto* lightingSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc, "RestirLighting");
            for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
            {
                lightingSubGraph->addReferencedResource(
                    gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)),
                    vkm::VkmResourceAccess::ShaderSampledRead);
            }
            lightingSubGraph->addReferencedResource(temporal.getReservoirBuffer(),
                                                    vkm::VkmResourceAccess::ShaderStorageReadWrite);
            lightingSubGraph->addReferencedResource(temporal.getLightingConstantBuffer(),
                                                    vkm::VkmResourceAccess::ConstantBufferRead);
            lightingSubGraph->setRenderCallback([&](vkm::VkmCommandBufferBase* commandBuffer) {
                temporal.recordLighting(commandBuffer);
            });

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            const std::vector<uint8_t> lightingBytes = detail::readTextureBytes(
                driver, target->getHandle(), static_cast<uint64_t>(pixelCount) * 4 * sizeof(uint16_t));
            const std::vector<uint8_t> motionBytes = detail::readTextureBytes(
                driver, gbuffer.getTexture(vkm::VkmGBuffer::Target::MotionMetallic),
                static_cast<uint64_t>(pixelCount) * 4 * sizeof(uint16_t));
            const std::vector<uint8_t> albedoBytes = detail::readTextureBytes(
                driver, gbuffer.getTexture(vkm::VkmGBuffer::Target::BaseColorRoughness),
                static_cast<uint64_t>(pixelCount) * 4);
            const uint16_t* lightingHalves = reinterpret_cast<const uint16_t*>(lightingBytes.data());
            const uint16_t* motionHalves = reinterpret_cast<const uint16_t*>(motionBytes.data());

            double lightingMean = 0.0;
            uint32_t lightingCovered = 0;
            uint32_t backgroundNonZero = 0;
            for (uint32_t pixel = 0; pixel < pixelCount; ++pixel)
            {
                const float irradiance = detail::halfToFloat(lightingHalves[pixel * 4 + 0]);
                const float coveredHere = detail::halfToFloat(motionHalves[pixel * 4 + 3]);
                if (coveredHere <= 0.0f)
                {
                    backgroundNonZero += irradiance != 0.0f ? 1 : 0;
                    continue;
                }
                const float albedo = static_cast<float>(albedoBytes[pixel * 4 + 0]) / 255.0f;
                const float metallic = detail::halfToFloat(motionHalves[pixel * 4 + 2]);
                lightingMean += irradiance * albedo * (1.0f - metallic) * (1.0f / 3.14159265f);
                ++lightingCovered;
            }
            lightingMean = lightingCovered > 0 ? lightingMean / lightingCovered : 0.0;

            MESSAGE("lighting texture mean (composited) " << lightingMean << " vs accumulated "
                                                          << temporalBrightness << ", ratio "
                                                          << (lightingMean / temporalBrightness));
            CHECK(lightingCovered == covered);
            CHECK(backgroundNonZero == 0);
            // One frame's estimate against a 1536-frame mean: the tolerance is the single-frame
            // noise of a confidence-21 reservoir averaged over ~2200 pixels, measured and given
            // 3x margin.
            CHECK(std::abs(lightingMean / temporalBrightness - 1.0) < 0.10);

            driver->waitIdle();
            driver->getRenderResourcePool()->releaseResource(target->getHandle());
        }

        // scene.destroy releases through the deferred reclaimer, whose worker frees on another
        // thread while the next test is already allocating; see TestAccelerationStructureShared.
        driver->waitIdle();
        temporal.destroy(driver);
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
