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
    inline void runAreaLightTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kSize = 128;

        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;
        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_emissive_plane.gltf",
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
        REQUIRE(scene.getLightTriangleCount() == 4);
        REQUIRE(scene.getLightTriangles().size() == 4);

        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, glm::uvec2(kSize, kSize)));

        // Pinhole, for the reason TestShadowedLightingShared records: the deferred pass
        // reconstructs world position by walking cameraDistance along a ray from the eye.
        const glm::vec3 eye(0.0f, 9.0f, 7.0f);
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
        REQUIRE(lightConstants._areaLightCount.x == 4u);
        REQUIRE(driver->uploadToBuffer(lightBuffer->getHandle(), &lightConstants, sizeof(lightConstants)));

        const std::string gbufferPsoName =
            std::string("gbuffer_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* gbufferPso =
            manager.getPipelineState(gbufferPsoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(gbufferPso != nullptr);
        vkm::VkmPipelineStateBase* lightingPso =
            manager.getPipelineState("deferred_lighting_pso", vkm::VkmPipelineStateOrigin::Engine);
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

        const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
        uint32_t checked = 0;
        double worstRelative = 0.0;
        for (uint32_t y = 0; y < kSize; ++y)
        {
            for (uint32_t x = 0; x < kSize; ++x)
            {
                // Reproduce this texel's floor point exactly as the rasterizer and the deferred
                // reconstruction did: the pixel centre's ray, intersected with y = 0.
                const float u = (static_cast<float>(x) + 0.5f) / kSize;
                const float v = (static_cast<float>(y) + 0.5f) / kSize;
                const glm::vec4 nearPoint =
                    inverseViewProjection * glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 0.0f, 1.0f);
                const glm::vec4 farPoint =
                    inverseViewProjection * glm::vec4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 1.0f, 1.0f);
                const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
                const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);

                // Skip texels the emitters themselves cover: those shade the emitter, not the
                // floor, and carry its emission besides.
                if (direction.y >= -1.0e-4f ||
                    needetail::segmentHitsRect(needetail::kEmitterA, origin, direction) ||
                    needetail::segmentHitsRect(needetail::kEmitterB, origin, direction))
                {
                    continue;
                }
                const float t = -origin.y / direction.y;
                const glm::vec3 hit = origin + direction * t;
                // Away from the floor edge, where a half-texel drift lands on background.
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

                const uint8_t* p =
                    &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
                const float measured = readHalfComponent(p, 0);
                const double relative = std::abs(measured - expected.x) / expected.x;
                worstRelative = std::max(worstRelative, relative);
                ++checked;
            }
        }

        MESSAGE("area light: " << checked << " floor texels, worst relative error " << worstRelative);
        CHECK(checked > 200);
        /*
        * The integral is exact, so what is left is the half-texel drift between the CPU's
        * reconstructed floor point and the GPU's, plus fp16 storage. Dropping the horizon clip,
        * the cosine weighting inside the edge sum, or the per-triangle accumulation each move
        * texels by far more than this.
        */
        CHECK(worstRelative < 0.03);

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
    }
} // namespace vkmtest
