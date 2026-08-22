// Copyright (c) 2026 Snowapril
//
// The next-event-estimation gates. Three claims, each pinned where it is cheapest to catch:
//
// 1. The area estimator's MEAN is right. A wrong pdf (broken CDF, missing area, dropped
//    cos_y/d^2) shifts the mean, which no convergence-style comparison can see -- both sides of
//    such a comparison share the estimator. So the reference is ANALYTIC: a diffuse floor under
//    two rectangular emitters has the closed-form value (albedo/pi) * sum(L_e * projected solid
//    angle), with the projected solid angle from the Lambert edge-sum formula. Two emitters of
//    unequal area and unequal strength on purpose: equal ones would let a uniform-triangle-pick
//    bug cancel out of the total.
//
// 2. The directional light reaches the traced tier, shadowed. One floor pixel in the open must
//    add exactly (albedo/pi) * cos * R; one under an emitter's footprint must not.
//
// 3. The emission-at-first-vertex convention holds in a closed scene: the emissive Cornell box is
//    lit only by its ceiling patch, and the deferred 1-spp estimator (emission-on-hit at its
//    first traced vertex) must converge to the reference (NEE at its primary hit) -- the two
//    cover the same integral through different halves of the convention, so a double count or a
//    dropped term separates them.
#pragma once

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/indirect_pass.h>
#include <vkm/renderer/path_tracer.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vkmtest
{
    namespace needetail
    {
        inline constexpr uint32_t kPlaneWidth = 32;
        inline constexpr uint32_t kPlaneHeight = 32;
        // Enough that the single-light-sample estimator's noise sits well under the analytic
        // tolerance below; the scene is tiny, so this is cheap.
        inline constexpr uint32_t kPlaneSamples = 4096;

        // The fixture's two emitters, mirrored from gltf_emissive_plane.gltf: corners wound so
        // consecutive edges circle the rectangle, radiance = factor * emissive strength.
        struct EmitterRect
        {
            glm::vec3 _corners[4];
            glm::vec3 _radiance;
        };
        inline const EmitterRect kEmitterA{
            { glm::vec3(-1.5f, 2.0f, -0.5f), glm::vec3(-0.5f, 2.0f, -0.5f),
              glm::vec3(-0.5f, 2.0f, 0.5f), glm::vec3(-1.5f, 2.0f, 0.5f) },
            glm::vec3(8.0f)
        };
        inline const EmitterRect kEmitterB{
            { glm::vec3(0.5f, 3.0f, -1.0f), glm::vec3(2.5f, 3.0f, -1.0f),
              glm::vec3(2.5f, 3.0f, 1.0f), glm::vec3(0.5f, 3.0f, 1.0f) },
            glm::vec3(3.0f)
        };
        inline constexpr float kFloorAlbedo = 0.5f;

        /*
        * The projected solid angle of a polygon from `point` with surface normal `normal`:
        * one half the sum over edges of the angle subtended times the z-component of the edge
        * plane's normal (Lambert's formula, exact for an unoccluded polygon).
        */
        inline float projectedSolidAngle(const EmitterRect& rect, const glm::vec3& point,
                                         const glm::vec3& normal)
        {
            float sum = 0.0f;
            for (uint32_t edge = 0; edge < 4; ++edge)
            {
                const glm::vec3 a = glm::normalize(rect._corners[edge] - point);
                const glm::vec3 b = glm::normalize(rect._corners[(edge + 1) % 4] - point);
                const float angle = std::acos(glm::clamp(glm::dot(a, b), -1.0f, 1.0f));
                const glm::vec3 edgeNormal = glm::normalize(glm::cross(a, b));
                sum += angle * glm::dot(normal, edgeNormal);
            }
            return std::abs(sum) * 0.5f;
        }

        // Whether the segment from `origin` along `direction` crosses `rect`'s horizontal plane
        // inside the rectangle -- used both to skip camera rays an emitter blocks and to find
        // pixels inside a sun shadow.
        inline bool segmentHitsRect(const EmitterRect& rect, const glm::vec3& origin,
                                    const glm::vec3& direction)
        {
            const float planeY = rect._corners[0].y;
            if (std::abs(direction.y) < 1.0e-6f)
            {
                return false;
            }
            const float t = (planeY - origin.y) / direction.y;
            if (t <= 0.0f)
            {
                return false;
            }
            const glm::vec3 hit = origin + direction * t;
            const float minX = rect._corners[0].x;
            const float maxX = rect._corners[2].x;
            const float minZ = rect._corners[0].z;
            const float maxZ = rect._corners[2].z;
            return hit.x >= minX && hit.x <= maxX && hit.z >= minZ && hit.z <= maxZ;
        }

        inline std::vector<float> readAccumulation(vkm::VkmDriverBase* driver,
                                                   vkm::VkmResourceHandle source,
                                                   uint32_t width, uint32_t height)
        {
            const uint64_t byteSize = static_cast<uint64_t>(width) * height * 4 * sizeof(float);
            vkm::VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
            stagingInfo._size = byteSize;
            stagingInfo._debugName = "NeeReadback";
            vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            REQUIRE(staging != nullptr);

            const vkm::VkmResourceHandle destination = staging->getHandle();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginTransferSubGraph("NeeReadback");
            subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->copyBuffer(source, destination, 0, 0, byteSize);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            std::vector<float> rgba(static_cast<size_t>(width) * height * 4);
            staging->invalidate(0, byteSize);
            std::memcpy(rgba.data(), staging->map(), byteSize);
            driver->waitIdle();
            driver->getRenderResourcePool()->releaseResource(destination);
            return rgba;
        }
    } // namespace needetail

    /*
    * Gates 1 and 2: the analytic plane. The reference runs at maxBounces = 1 with a black
    * environment, so a floor pixel's value is PURE next-event estimation at the primary hit --
    * the exact quantity the closed form predicts.
    */
    inline void runNeeAnalyticPlaneTest(vkm::VkmDriverBase* driver)
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
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_emissive_plane.gltf",
                                             &model, &error, importOptions),
                        error);

        vkm::VkmPipelineStateManager manager(driver);
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &error),
                        error);

        // The sun: straight up, so a floor pixel's cosine is exactly 1 and the emitters' own
        // footprints are exactly the shadowed regions.
        const glm::vec3 sunRadiance(2.0f, 2.0f, 2.0f);
        const glm::vec3 sunDirection(0.0f, 1.0f, 0.0f);

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        scene.setDirectionalRadiance(sunRadiance);
        REQUIRE_MESSAGE(scene.build(driver, &manager, &error), error);
        // Two emitters, two triangles each; a wrong gather count here fails before any GPU work.
        CHECK(scene.getLightTriangleCount() == 4);
        REQUIRE_MESSAGE(scene.buildAccelerationStructures(driver, &error), error);

        REQUIRE_MESSAGE(vkm::vkmLoadRayTracingPipelineStates(&manager, &error), error);

        vkm::VkmPathTracer tracer;
        REQUIRE_MESSAGE(tracer.initialize(driver, &manager, needetail::kPlaneWidth,
                                          needetail::kPlaneHeight, &error),
                        error);

        // From high above at a slant, looking at the floor centre: floor everywhere, the emitters
        // covering only small patches of frame.
        const glm::vec3 eye(0.0f, 9.0f, 7.0f);
        const glm::mat4 view = glm::lookAtRH(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
        const glm::mat4 viewProjection = projection * view;
        const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);

        vkm::VkmFrameConstants frameConstants{};
        frameConstants._view = view;
        frameConstants._projection = projection;
        frameConstants._viewProjection = viewProjection;
        frameConstants._inverseViewProjection = inverseViewProjection;
        frameConstants._prevViewProjection = viewProjection;
        frameConstants._cameraPositionWorld = glm::vec4(eye, 1.0f);
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);
        frameData._lightDirection = glm::vec4(sunDirection, 0.0f);

        // maxBounces 1 and a black environment: a floor pixel is NEE at the primary hit and
        // nothing else. Unjittered, so a pixel centre's ray is reproducible on the CPU below.
        const vkm::VkmPathTraceOptions options{
            /*_maxBounces=*/1,
            /*_jitterPrimaryRay=*/false,
            glm::vec3(0.0f)
        };

        std::vector<vkm::VkmResourceAccessDeclaration> referenced;
        for (uint32_t sample = 0; sample < needetail::kPlaneSamples; ++sample)
        {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("NeeSceneUpdate");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Update, &referenced);
            updateSubGraph->addReferencedResources(referenced);
            updateSubGraph->setTransferCallback([&scene, frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });
            auto* traceSubGraph = renderGraph.beginComputeSubGraph("NeeTrace");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
            traceSubGraph->addReferencedResources(referenced);
            traceSubGraph->addReferencedResource(scene.getTopLevelAccelerationStructure(),
                                                vkm::VkmResourceAccess::AccelerationStructureShaderRead);
            traceSubGraph->addReferencedResource(tracer.getAccumulationBuffer(),
                                                vkm::VkmResourceAccess::ShaderStorageReadWrite);
            traceSubGraph->setComputeCallback([&tracer, options](vkm::VkmCommandBufferBase* commandBuffer) {
                tracer.recordAccumulate(commandBuffer, options);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }

        const std::vector<float> image = needetail::readAccumulation(
            driver, tracer.getAccumulationBuffer(), needetail::kPlaneWidth, needetail::kPlaneHeight);

        /*
        * Walk every pixel whose centre ray reaches the FLOOR unobstructed, reproduce the primary
        * hit on the CPU, and compare against the closed form. Sun-shadowed pixels (inside an
        * emitter's footprint) drop the sun term, which is gate 2 -- and the fixture guarantees
        * both kinds exist, asserted below rather than assumed.
        */
        uint32_t litChecked = 0;
        uint32_t shadowChecked = 0;
        double worstRelative = 0.0;
        for (uint32_t y = 0; y < needetail::kPlaneHeight; ++y)
        {
            for (uint32_t x = 0; x < needetail::kPlaneWidth; ++x)
            {
                // The same pixel-centre NDC the tracer uses, unjittered.
                const float u = (static_cast<float>(x) + 0.5f) / needetail::kPlaneWidth;
                const float v = (static_cast<float>(y) + 0.5f) / needetail::kPlaneHeight;
                const glm::vec4 nearPoint =
                    inverseViewProjection * glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 0.0f, 1.0f);
                const glm::vec4 farPoint =
                    inverseViewProjection * glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 1.0f, 1.0f);
                const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
                const glm::vec3 direction =
                    glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);

                // Floor intersection, skipped when an emitter is in the way of the camera.
                if (direction.y >= -1.0e-4f ||
                    needetail::segmentHitsRect(needetail::kEmitterA, origin, direction) ||
                    needetail::segmentHitsRect(needetail::kEmitterB, origin, direction))
                {
                    continue;
                }
                const float t = -origin.y / direction.y;
                const glm::vec3 hit = origin + direction * t;
                if (std::abs(hit.x) > 3.8f || std::abs(hit.z) > 3.8f)
                {
                    continue; // near the floor edge, where a half-pixel ray drift may miss
                }

                const glm::vec3 up(0.0f, 1.0f, 0.0f);
                glm::vec3 expected(0.0f);
                expected += needetail::kEmitterA._radiance *
                            (needetail::kFloorAlbedo / 3.14159265f) *
                            needetail::projectedSolidAngle(needetail::kEmitterA, hit, up);
                expected += needetail::kEmitterB._radiance *
                            (needetail::kFloorAlbedo / 3.14159265f) *
                            needetail::projectedSolidAngle(needetail::kEmitterB, hit, up);

                // The sun term, unless this point sits inside an emitter's shadow footprint.
                const bool shadowed =
                    needetail::segmentHitsRect(needetail::kEmitterA, hit, up) ||
                    needetail::segmentHitsRect(needetail::kEmitterB, hit, up);
                if (!shadowed)
                {
                    expected += sunRadiance * (needetail::kFloorAlbedo / 3.14159265f);
                    ++litChecked;
                }
                else
                {
                    ++shadowChecked;
                }

                const size_t pixel = (static_cast<size_t>(y) * needetail::kPlaneWidth + x) * 4;
                const float samples = image[pixel + 3];
                REQUIRE(samples > 0.0f);
                const float measured = image[pixel + 0] / samples;
                const double relative =
                    std::abs(measured - expected.x) / std::max(expected.x, 1.0e-4f);
                worstRelative = std::max(worstRelative, relative);
            }
        }
        MESSAGE("analytic NEE plane: " << litChecked << " lit + " << shadowChecked
                                       << " shadowed pixels, worst relative error " << worstRelative);
        // Both branches of the closed form must actually run, or the gate proves half of what it
        // claims: the fixture is arranged so both exist in frame.
        CHECK(litChecked > 50);
        CHECK(shadowChecked > 5);
        /*
        * Measured then margined: the single-light-sample estimator at 4096 samples lands well
        * inside this on both backends; a dropped cos_y/d^2, a broken CDF, an unshadowed sun or a
        * missing shadow ray each move whole pixels by tens of percent.
        */
        CHECK(worstRelative < 0.05);

        driver->waitIdle();
        tracer.destroy(driver);
        scene.destroy(driver);
        driver->getDeferredReclaimer()->flushBlocking();
    }

    /*
    * Gate 3: the emissive Cornell box. The deferred 1-spp estimator finds the ceiling patch by
    * hitting it (emission at its first traced vertex); the reference finds it by NEE at the
    * primary hit. Same integral, opposite halves of the convention -- a double count or a dropped
    * term separates the two means. The camera must not see the patch itself: the G-buffer's
    * emissive target is read back to ASSERT that, because gi_indirect excludes the primary
    * surface's own emission (the deferred composite adds it) while the reference includes it.
    */
    inline void runNeeEmissiveCornellTest(vkm::VkmDriverBase* driver, float mseThreshold)
    {
        REQUIRE(driver != nullptr);
        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            MESSAGE("Skipping: this backend reports no RayTracing capability.");
            return;
        }

        constexpr uint32_t kSize = 48;
        constexpr uint32_t kSamples = 1536;
        constexpr uint32_t kBounces = 3;

        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_cornell_emissive.gltf",
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
        // The ceiling patch's two triangles, and nothing else.
        CHECK(scene.getLightTriangleCount() == 2);
        REQUIRE_MESSAGE(scene.buildAccelerationStructures(driver, &error), error);

        // Inside the box near its open front, pitched down so the ceiling patch stays out of
        // frame -- asserted below through the emissive target, not trusted.
        const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.2f, 0.85f),
                                             glm::vec3(0.0f, -0.35f, -1.0f),
                                             glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.05f, 20.0f);
        const glm::mat4 viewProjection = projection * view;

        vkm::VkmFrameConstants frameConstants{};
        frameConstants._view = view;
        frameConstants._projection = projection;
        frameConstants._viewProjection = viewProjection;
        frameConstants._inverseViewProjection = glm::inverse(viewProjection);
        frameConstants._prevViewProjection = viewProjection;
        frameConstants._cameraPositionWorld = glm::vec4(0.0f, 0.2f, 0.85f, 1.0f);
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);

        std::vector<vkm::VkmResourceAccessDeclaration> referenced;

        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, glm::uvec2(kSize, kSize)));

        REQUIRE_MESSAGE(vkm::vkmLoadRayTracingPipelineStates(&manager, &error), error);

        vkm::VkmPathTracer reference;
        REQUIRE_MESSAGE(reference.initialize(driver, &manager, kSize, kSize, &error), error);
        vkm::VkmIndirectPass indirect;
        REQUIRE_MESSAGE(indirect.initialize(driver, &manager, gbuffer, kSize, kSize, &error), error);

        const vkm::VkmPathTraceOptions referenceOptions{
            /*_maxBounces=*/kBounces + 1,
            /*_jitterPrimaryRay=*/false,
            glm::vec3(0.0f) // the patch is the only light
        };
        const vkm::VkmIndirectOptions indirectOptions{ kBounces, glm::vec3(0.0f) };

        // One G-buffer fill; the camera and scene are static.
        {
            const vkm::VkmFrameBufferDescriptor fbDesc = gbuffer.makeFrameBufferDescriptor();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("EmissiveCornellUpdate");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Update, &referenced);
            updateSubGraph->addReferencedResources(referenced);
            updateSubGraph->setTransferCallback([&scene, frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });
            auto* cullSubGraph = renderGraph.beginComputeSubGraph("EmissiveCornellCull");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Cull, &referenced);
            cullSubGraph->addReferencedResources(referenced);
            cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordCull(commandBuffer, /*viewIndex=*/0);
            });
            auto* gbufferSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc, "EmissiveCornellGBuffer");
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
        }

        // The precondition, checked rather than trusted: no covered pixel may see the emitter,
        // because the deferred pass adds primary-surface emission in its composite while
        // gi_indirect's estimator excludes it -- a visible patch would make the two measure
        // different quantities by construction.
        {
            const vkm::VkmTextureReadbackResult emissive =
                driver->readbackTexture(gbuffer.getTexture(vkm::VkmGBuffer::Target::Emissive));
            REQUIRE(emissive.channels == 8);
            uint32_t emissivePixels = 0;
            for (uint32_t pixel = 0; pixel < kSize * kSize; ++pixel)
            {
                const uint16_t half0 = *reinterpret_cast<const uint16_t*>(
                    &emissive.pixels[static_cast<size_t>(pixel) * emissive.channels]);
                if ((half0 & 0x7FFFu) != 0)
                {
                    ++emissivePixels;
                }
            }
            REQUIRE(emissivePixels == 0);
        }

        for (uint32_t sample = 0; sample < kSamples; ++sample)
        {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginComputeSubGraph("EmissiveCornellEstimators");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
            subGraph->addReferencedResources(referenced);
            subGraph->addReferencedResource(scene.getTopLevelAccelerationStructure(),
                                           vkm::VkmResourceAccess::AccelerationStructureShaderRead);
            for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
            {
                subGraph->addReferencedResource(gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)),
                                                vkm::VkmResourceAccess::ShaderSampledRead);
            }
            subGraph->addReferencedResource(reference.getAccumulationBuffer(),
                                            vkm::VkmResourceAccess::ShaderStorageReadWrite);
            subGraph->addReferencedResource(indirect.getAccumulationBuffer(),
                                            vkm::VkmResourceAccess::ShaderStorageReadWrite);
            subGraph->setComputeCallback([&](vkm::VkmCommandBufferBase* commandBuffer) {
                reference.recordAccumulate(commandBuffer, referenceOptions);
                indirect.recordAccumulate(commandBuffer, indirectOptions);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }

        const std::vector<float> referenceImage = needetail::readAccumulation(
            driver, reference.getAccumulationBuffer(), kSize, kSize);
        const std::vector<float> indirectImage = needetail::readAccumulation(
            driver, indirect.getAccumulationBuffer(), kSize, kSize);
        const uint32_t pixelCount = kSize * kSize;

        uint32_t covered = 0;
        double referenceMean = 0.0;
        double indirectMean = 0.0;
        for (uint32_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            if (indirectImage[pixel * 4 + 3] > 0.0f && referenceImage[pixel * 4 + 3] > 0.0f)
            {
                referenceMean += referenceImage[pixel * 4 + 0] / referenceImage[pixel * 4 + 3];
                indirectMean += indirectImage[pixel * 4 + 0] / indirectImage[pixel * 4 + 3];
                ++covered;
            }
        }
        REQUIRE(covered > pixelCount / 2);
        referenceMean /= covered;
        indirectMean /= covered;

        const float mse = vkm::vkmComputeImageMse(referenceImage.data(), indirectImage.data(), pixelCount);
        const float relativeMse =
            vkm::vkmComputeImageRelativeMse(referenceImage.data(), indirectImage.data(), pixelCount);
        MESSAGE("emissive Cornell: MSE " << mse << ", RelMSE " << relativeMse << ", mean ref "
                                         << referenceMean << " vs indirect " << indirectMean
                                         << ", ratio " << (indirectMean / referenceMean));
        // Lit by the patch alone, so "they agree" is a statement about the emitter's light.
        CHECK(referenceMean > 0.01);
        CHECK(std::abs(indirectMean / referenceMean - 1.0) < 0.02);
        CHECK(mse < mseThreshold);

        driver->waitIdle();
        indirect.destroy(driver);
        reference.destroy(driver);
        gbuffer.destroy();
        scene.destroy(driver);
        driver->getDeferredReclaimer()->flushBlocking();
    }
} // namespace vkmtest
