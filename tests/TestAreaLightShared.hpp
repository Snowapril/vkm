// Copyright (c) 2026 Snowapril
//
// The raster tier's area-light gate.
//
// One claim, pinned where it is cheapest to catch: the deferred pass's polygon integral returns
// the RIGHT AMOUNT of light, not merely some light. That has to be checked against a closed form
// rather than against the traced tier, because both would then share the thing under test -- and
// because the failure mode is a wrong mean, which no "does it look lit" check can see.
//
// The reference is the same Lambert edge-sum the NEE gate uses (needetail::projectedSolidAngle),
// evaluated on the fixture's two emitter RECTANGLES while the shader integrates the four
// TRIANGLES they decompose into. That difference is the point: projected solid angle is additive
// over a partition, so agreeing to a closed form computed on the undivided rectangle also proves
// the triangle decomposition and the per-triangle accumulation.
//
// Deliberately no sun and no punctual lights, so a floor texel's entire value is the area term.
#pragma once

#include "TestHalfFloatShared.hpp"
#include "TestNeeShared.hpp"
#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/deferred_lighting.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/shadow_atlas.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <string>
#include <vector>

namespace vkmtest
{
    // What a gate needs back from one render: the image, the camera it was taken with, and the
    // emitters the shader was handed.
    struct AreaLightRender
    {
        std::vector<uint8_t> _pixels;
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint32_t _channels = 0;
        glm::mat4 _inverseViewProjection{ 1.0f };
        glm::vec3 _eye{ 0.0f };
        std::vector<vkm::VkmLightTableTriangle> _triangles;
    };

    /*
    * @brief Renders one emissive fixture through the G-buffer and the deferred pass.
    * @param fixture Path under resources/tests.
    * @param eye Camera position; it always looks at the origin.
    * @param expectedTriangles Emissive triangles the fixture must produce, asserted before any
    * GPU work so a gather regression fails where it happened rather than as a wrong image.
    */
    inline AreaLightRender renderAreaLightScene(vkm::VkmDriverBase* driver, const char* fixture,
                                                const glm::vec3& eye, uint32_t expectedTriangles)
    {
        constexpr uint32_t kSize = 128;

        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;
        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/" + fixture,
                                             &model, &error, importOptions),
                        error);

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        // No setDirectionalLight: the sun would add a term this gate does not model.
        REQUIRE_MESSAGE(scene.build(driver, &manager, &error), error);
        // Two rectangles, two triangles each. A gather that lost one would halve an emitter's
        // contribution, which the closed form below would catch anyway -- but failing here says
        // why.
        REQUIRE(scene.getLightTriangleCount() == expectedTriangles);
        REQUIRE(scene.getLightTriangles().size() == expectedTriangles);

        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, glm::uvec2(kSize, kSize)));

        // Pinhole, for the reason TestShadowedLightingShared records: the deferred pass
        // reconstructs world position by walking cameraDistance along a ray from the eye.
        const glm::mat4 view = glm::lookAtRH(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
        const glm::mat4 viewProjection = projection * view;

        vkm::VkmFrameConstants frameConstants{};
        frameConstants._view = view;
        frameConstants._projection = projection;
        frameConstants._viewProjection = viewProjection;
        frameConstants._inverseViewProjection = glm::inverse(viewProjection);
        frameConstants._viewProjectionNoJitter = viewProjection;
        frameConstants._prevViewProjection = viewProjection;
        frameConstants._cameraPositionWorld = glm::vec4(eye, 1.0f);
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);

        // The deferred PSO's table declares the atlas bindings whether or not anything casts a
        // shadow, so one exists with no lights assigned to it.
        std::vector<vkm::VkmPunctualLight> noLights;
        vkm::VkmShadowAtlas atlas;
        vkm::VkmShadowAtlas::Descriptor atlasDescriptor;
        atlasDescriptor._tileSize = 256u;
        atlasDescriptor._cullViewIndex = 2u;
        std::string atlasError;
        REQUIRE_MESSAGE(atlas.initialize(driver, &manager, atlasDescriptor, &atlasError), atlasError);
        REQUIRE_MESSAGE(atlas.prepareScene(scene, &atlasError), atlasError);
        atlas.allocate(scene, &noLights);

        vkm::VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "AreaLightSampler";
        vkm::VkmSampler* sampler = driver->newSampler(samplerInfo);
        REQUIRE(sampler != nullptr);

        vkm::VkmTextureInfo targetInfo{};
        targetInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                            vkm::VkmResourceCreateInfo::AllowTransferSrc;
        targetInfo._extent = glm::uvec3(kSize, kSize, 1);
        targetInfo._numMipLevels = 1;
        targetInfo._numArrayLayers = 1;
        targetInfo._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        targetInfo._debugName = "AreaLightTarget";
        vkm::VkmTexture* target = driver->newTexture(targetInfo);
        REQUIRE(target != nullptr);

        vkm::VkmBufferInfo lightBufferInfo{};
        lightBufferInfo._flags =
            vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
        lightBufferInfo._size = sizeof(vkm::VkmDeferredLightConstants);
        lightBufferInfo._debugName = "AreaLightConstants";
        vkm::VkmBuffer* lightBuffer = driver->newBuffer(lightBufferInfo);
        REQUIRE(lightBuffer != nullptr);

        vkm::VkmDeferredLightConstants lightConstants{};
        vkm::vkmBuildDeferredLightConstants(scene, &lightConstants);
        // Nothing punctual: no sun was set and the fixture places no lights.
        REQUIRE(lightConstants._lightCount.x == 0u);
        REQUIRE(lightConstants._areaLightCount.x == expectedTriangles);
        REQUIRE(driver->uploadToBuffer(lightBuffer->getHandle(), &lightConstants, sizeof(lightConstants)));

        const std::string gbufferPsoName =
            std::string("gbuffer_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* gbufferPso =
            manager.getPipelineState(gbufferPsoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(gbufferPso != nullptr);
        vkm::VkmPipelineStateBase* lightingPso =
            manager.getPipelineState("deferred_lighting_pso[area]", vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(lightingPso != nullptr);

        vkm::VkmFrameBufferDescriptor lightingFb{};
        lightingFb._width = kSize;
        lightingFb._height = kSize;
        lightingFb._renderPass._colorAttachmentCount = 1;
        lightingFb._renderPass._colorAttachments[0]._attachmentId = 0;
        lightingFb._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        lightingFb._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        lightingFb._colorAttachments[0] = target->getHandle();

        const std::vector<vkm::VkmTableResourceEntry> entries{
            { 0, gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal) },
            { 1, gbuffer.getTexture(vkm::VkmGBuffer::Target::BaseColorRoughness) },
            { 2, gbuffer.getTexture(vkm::VkmGBuffer::Target::MotionMetallic) },
            { 3, sampler->getHandle() },
            { 4, lightBuffer->getHandle() },
            { 5, gbuffer.getTexture(vkm::VkmGBuffer::Target::Emissive) },
            { 6, atlas.getAtlasTexture() },
            { 7, atlas.getConstantBuffer() },
        };
        std::string tableError;
        vkm::VkmResourceTableBase* table =
            driver->newResourceTable(lightingPso, vkm::VkmResourceSetKind::PerPass, entries, &tableError);
        REQUIRE_MESSAGE(table != nullptr, tableError);

        {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            std::vector<vkm::VkmResourceAccessDeclaration> referenced;
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("AreaSceneUpdate");
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Update, &referenced);
            updateSubGraph->addReferencedResources(referenced);
            updateSubGraph->setTransferCallback([&scene, &frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });

            auto* cullSubGraph = renderGraph.beginComputeSubGraph("AreaSceneCull");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Cull, &referenced);
            cullSubGraph->addReferencedResources(referenced);
            cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordCull(commandBuffer, /*viewIndex=*/0);
            });

            auto* drawSubGraph = renderGraph.beginGraphicsSubGraph(gbuffer.makeFrameBufferDescriptor());
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
            drawSubGraph->addReferencedResources(referenced);
            drawSubGraph->setRenderCallback([&scene, gbufferPso](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordDrawBatches(commandBuffer,
                                        [gbufferPso](const vkm::VkmScene::DrawBatch&) { return gbufferPso; },
                                        {}, /*viewIndex=*/0);
            });

            auto* lightingSubGraph = renderGraph.beginGraphicsSubGraph(lightingFb);
            lightingSubGraph->addReferencedResource(target->getHandle(),
                                                    vkm::VkmResourceAccess::ColorAttachmentWrite);
            std::vector<vkm::VkmResourceAccessDeclaration> bound;
            table->collectReferencedResources(&bound);
            lightingSubGraph->addReferencedResources(bound);
            lightingSubGraph->setRenderCallback([lightingPso, table](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(lightingPso);
                commandBuffer->bindResourceTable(table);
                commandBuffer->draw(3, 1, 0, 0);
            });

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }

        const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(target->getHandle());
        REQUIRE(readback.channels == 8); // RGBA16F

        AreaLightRender result;
        result._pixels = readback.pixels;
        result._width = readback.width;
        result._height = readback.height;
        result._channels = readback.channels;
        result._inverseViewProjection = glm::inverse(viewProjection);
        result._eye = eye;
        result._triangles = scene.getLightTriangles();

        driver->waitIdle();
        table->destroy();
        delete table;
        atlas.destroy();
        gbuffer.destroy();
        scene.destroy(driver);
        for (vkm::VkmResourceHandle handle : { target->getHandle(), lightBuffer->getHandle(),
                                               sampler->getHandle() })
        {
            driver->getRenderResourcePool()->releaseResource(handle);
        }
        driver->getDeferredReclaimer()->flushBlocking();
        return result;
    }

    /*
    * @brief Projected solid angle of a triangle at a point, by integrating over its AREA.
    * @details The independent reference. Deliberately a different derivation from the shader's:
    * the shader walks the polygon's edges (Lambert's formula) while this sums
    * cos_x * |cos_y| / d^2 * dA over the surface. Agreeing means two derivations agree, which an
    * edge-sum reference restating the shader's own algorithm could never show.
    *
    * The horizon clip falls out of the `cos_x > 0` test here rather than being a separate step,
    * which is exactly why this reference can see a missing clip: a polygon crossing the tangent
    * plane contributes only its upper part, with no sign to get wrong.
    *
    * Convergence degrades as the shading point approaches the emitter (1/d^2 sharpens), so
    * callers keep their sample points at a distance.
    */
    inline float numericFormFactor(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                                   const glm::vec3& point, const glm::vec3& normal, uint32_t steps)
    {
        const glm::vec3 e1 = p1 - p0;
        const glm::vec3 e2 = p2 - p0;
        const glm::vec3 cross = glm::cross(e1, e2);
        const float area = 0.5f * glm::length(cross);
        if (area <= 0.0f)
        {
            return 0.0f;
        }
        const glm::vec3 lightNormal = cross / (2.0f * area);

        double sum = 0.0;
        uint32_t accepted = 0;
        for (uint32_t i = 0; i < steps; ++i)
        {
            for (uint32_t j = 0; j < steps; ++j)
            {
                const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
                const float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(steps);
                if (u + v > 1.0f)
                {
                    continue; // outside the triangle
                }
                ++accepted;
                const glm::vec3 onLight = p0 + e1 * u + e2 * v;
                const glm::vec3 toLight = onLight - point;
                const float distanceSquared = glm::dot(toLight, toLight);
                if (distanceSquared <= 1.0e-8f)
                {
                    continue;
                }
                const glm::vec3 direction = toLight / std::sqrt(distanceSquared);
                const float cosSurface = glm::dot(normal, direction);
                if (cosSurface <= 0.0f)
                {
                    continue; // below the horizon -- the clip, expressed as the integrand
                }
                // Two-sided, matching both tiers.
                const float cosLight = std::abs(glm::dot(lightNormal, -direction));
                sum += static_cast<double>(cosSurface) * static_cast<double>(cosLight) /
                       static_cast<double>(distanceSquared);
            }
        }
        if (accepted == 0)
        {
            return 0.0f;
        }
        return static_cast<float>(sum * (static_cast<double>(area) / static_cast<double>(accepted)));
    }


    /*
    * Gate 1: emitters wholly above the receiver plane, checked against the Lambert edge-sum the
    * NEE gate already uses. Broad coverage -- thousands of texels -- but it cannot reach the
    * horizon clip, because nothing here ever straddles.
    */
    inline void runAreaLightTest(vkm::VkmDriverBase* driver)
    {
        const AreaLightRender render =
            renderAreaLightScene(driver, "gltf_emissive_plane.gltf", glm::vec3(0.0f, 9.0f, 7.0f), 4u);

        uint32_t checked = 0;
        double worstRelative = 0.0;
        // Every 2nd texel on each axis: the numeric specular reference is the expensive half, and
        // a quarter of the frame still leaves an order of magnitude more than the floor below.
        for (uint32_t y = 0; y < render._height; y += 2)
        {
            for (uint32_t x = 0; x < render._width; x += 2)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(render._width);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(render._height);
                const glm::vec4 nearPoint = render._inverseViewProjection *
                                            glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 0.0f, 1.0f);
                const glm::vec4 farPoint = render._inverseViewProjection *
                                           glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 1.0f, 1.0f);
                const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
                const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);

                if (direction.y >= -1.0e-4f ||
                    needetail::segmentHitsRect(needetail::kEmitterA, origin, direction) ||
                    needetail::segmentHitsRect(needetail::kEmitterB, origin, direction))
                {
                    continue;
                }
                const float t = -origin.y / direction.y;
                const glm::vec3 hit = origin + direction * t;
                if (std::abs(hit.x) > 3.5f || std::abs(hit.z) > 3.5f)
                {
                    continue;
                }

                const glm::vec3 up(0.0f, 1.0f, 0.0f);
                glm::vec3 expected(0.0f);
                expected += needetail::kEmitterA._radiance * (needetail::kFloorAlbedo / 3.14159265f) *
                            needetail::projectedSolidAngle(needetail::kEmitterA, hit, up);
                expected += needetail::kEmitterB._radiance * (needetail::kFloorAlbedo / 3.14159265f) *
                            needetail::projectedSolidAngle(needetail::kEmitterB, hit, up);
                if (expected.x <= 1.0e-3f)
                {
                    continue;
                }

                const uint8_t* p = &render._pixels[(static_cast<size_t>(y) * render._width + x) *
                                                   render._channels];
                const float measured = readHalfComponent(p, 0);
                worstRelative =
                    std::max(worstRelative, static_cast<double>(std::abs(measured - expected.x) / expected.x));
                ++checked;
            }
        }

        MESSAGE("area light: " << checked << " floor texels, worst relative error " << worstRelative);
        CHECK(checked > 200);
        /*
        * The integral is exact, so what remains is the half-texel drift between the CPU's
        * reconstructed floor point and the GPU's, plus fp16 storage.
        */
        CHECK(worstRelative < 0.03);
    }

    /*
    * Gate 2: the horizon clip, which gate 1 provably cannot reach -- disabling the clip leaves
    * that one passing. Here an emissive panel stands IN the floor, half of it below, so every
    * floor point sees a polygon crossing its tangent plane and the clip runs on every texel.
    *
    * Checked against the numeric area integral rather than the edge sum, because the edge sum is
    * the thing under test.
    */
    inline void runAreaLightHorizonClipTest(vkm::VkmDriverBase* driver)
    {
        constexpr float kFloorAlbedo = 0.5f; // gltf_emissive_straddle.gltf's floor
        constexpr float kPanelX = 1.5f;      // the plane the panel stands in
        const AreaLightRender render = renderAreaLightScene(
            driver, "gltf_emissive_straddle.gltf", glm::vec3(-6.0f, 7.0f, 6.0f), 2u);
        REQUIRE(render._triangles.size() == 2);

        uint32_t checked = 0;
        double worstRelative = 0.0;
        // Every 4th texel: the numeric reference is the expensive half, and the claim is about the
        // clip rather than about coverage.
        for (uint32_t y = 0; y < render._height; y += 4)
        {
            for (uint32_t x = 0; x < render._width; x += 4)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(render._width);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(render._height);
                const glm::vec4 nearPoint = render._inverseViewProjection *
                                            glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 0.0f, 1.0f);
                const glm::vec4 farPoint = render._inverseViewProjection *
                                           glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 1.0f, 1.0f);
                const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
                const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);
                if (direction.y >= -1.0e-4f)
                {
                    continue;
                }
                const float t = -origin.y / direction.y;
                const glm::vec3 hit = origin + direction * t;
                // On the lit side of the panel and away from it: the reference's 1/d^2 sharpens as
                // the point approaches, and the panel occludes the camera besides.
                if (hit.x > kPanelX - 1.0f || hit.x < -3.5f || std::abs(hit.z) > 3.5f)
                {
                    continue;
                }

                const glm::vec3 up(0.0f, 1.0f, 0.0f);
                float formFactor = 0.0f;
                for (const vkm::VkmLightTableTriangle& tri : render._triangles)
                {
                    const glm::vec3 a(tri._p0[0], tri._p0[1], tri._p0[2]);
                    const glm::vec3 b(tri._p1[0], tri._p1[1], tri._p1[2]);
                    const glm::vec3 c(tri._p2[0], tri._p2[1], tri._p2[2]);
                    formFactor += numericFormFactor(a, b, c, hit, up, /*steps=*/96u);
                }
                const float radiance = render._triangles[0]._radiance[0];
                const float expected = radiance * (kFloorAlbedo / 3.14159265f) * formFactor;
                if (expected <= 1.0e-3f)
                {
                    continue;
                }

                const uint8_t* p = &render._pixels[(static_cast<size_t>(y) * render._width + x) *
                                                   render._channels];
                const float measured = readHalfComponent(p, 0);
                worstRelative =
                    std::max(worstRelative, static_cast<double>(std::abs(measured - expected) / expected));
                ++checked;
            }
        }

        MESSAGE("area light horizon clip: " << checked << " floor texels, worst relative error "
                                            << worstRelative);
        CHECK(checked > 100);
        /*
        * Looser than gate 1 because the reference is numeric: a 96x96 grid over each triangle
        * carries its own quadrature error. Wide enough to absorb that, far tighter than the tens
        * of percent an unclipped edge sum produces here.
        */
        CHECK(worstRelative < 0.06);
    }
} // namespace vkmtest
