#ifndef TEST_SHADOWED_LIGHTING_SHARED_HPP
#define TEST_SHADOWED_LIGHTING_SHARED_HPP

#include <doctest/doctest.h>

#include "TestHalfFloatShared.hpp"

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
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
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/shadow_atlas.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <string>
#include <vector>

namespace vkmtest
{
    /*
    * @brief The shadow term, measured against geometry whose shadow edge is known exactly.
    *
    * The fixture is a floor at y = 0 spanning +-4 with a square occluder at y = 2 spanning +-1.
    * The light sits OFF the occluder's axis, at (3, 6, 0), so the shadow lands on floor the
    * camera can see: a light directly overhead would hide its own umbra behind the occluder,
    * since a top-down view sees the occluder's lit top face there and never the floor beneath.
    * The rays through the occluder's two edges, (1, 2) and (-1, 2), reach y = 0 at x = 0 and
    * x = -3, so the umbra on the floor is exactly x in [-3, 0] at z = 0, of which x in [-3, -1]
    * is floor the camera can see.
    *
    * The comparison is the same texel with the light's shadow tile assigned and with it set to
    * -1. Attenuation, nDotL, albedo and the BRDF are then bit-identical between the two runs and
    * the ONLY difference is the visibility term -- which is what makes this a shadow test rather
    * than a lighting test that happens to get darker.
    */
    inline void runShadowedLightingTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kSize = 128;

        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;
        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_shadow_occluder.gltf",
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
        REQUIRE_MESSAGE(scene.build(driver, &manager, &error), error);

        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, glm::uvec2(kSize, kSize)));

        // Perspective, not orthographic, and that is not a stylistic choice: the deferred pass
        // reconstructs a world position by walking `cameraDistance` along the ray from
        // cameraPositionWorld through the pixel (vkmReconstructWorldPosition). That is a pinhole
        // construction. Under an orthographic projection every ray is parallel instead of sharing
        // an origin, so the reconstruction is correct only at the image centre and drifts further
        // out -- which reads exactly like a broken shadow lookup.
        const glm::vec3 eye(0.0f, 12.0f, 10.0f);
        const glm::mat4 view = glm::lookAtRH(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.5f, 60.0f);
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

        // Off the occluder's axis; see the note above.
        std::vector<vkm::VkmPunctualLight> lights(1);
        vkm::VkmPunctualLight& point = lights[0];
        point._type = static_cast<uint32_t>(vkm::VkmLightType::Point);
        point._positionWorld[0] = 3.0f;
        point._positionWorld[1] = 6.0f;
        point._positionWorld[2] = 0.0f;
        point._range = 0.0f; // unlimited, so the range window never enters the comparison
        // Scaled so a floor texel lands in a comfortable half-float range after 1/d^2.
        point._radiance[0] = 200.0f;
        point._radiance[1] = 200.0f;
        point._radiance[2] = 200.0f;

        vkm::VkmShadowAtlas atlas;
        vkm::VkmShadowAtlas::Descriptor atlasDescriptor;
        atlasDescriptor._tileSize = 512u;
        atlasDescriptor._cullViewIndex = 2u;
        std::string atlasError;
        REQUIRE_MESSAGE(atlas.initialize(driver, &manager, atlasDescriptor, &atlasError), atlasError);
        REQUIRE_MESSAGE(atlas.prepareScene(scene, &atlasError), atlasError);
        atlas.allocate(scene, &lights);
        // A point light owns six tiles, one per cube face.
        REQUIRE(atlas.getTileCount() == 6);
        REQUIRE(lights[0]._shadowTile == 0);

        vkm::VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "ShadowedLightingSampler";
        vkm::VkmSampler* sampler = driver->newSampler(samplerInfo);
        REQUIRE(sampler != nullptr);

        vkm::VkmTextureInfo targetInfo{};
        targetInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                            vkm::VkmResourceCreateInfo::AllowTransferSrc;
        targetInfo._extent = glm::uvec3(kSize, kSize, 1);
        targetInfo._numMipLevels = 1;
        targetInfo._numArrayLayers = 1;
        targetInfo._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        targetInfo._debugName = "ShadowedLightingTarget";
        vkm::VkmTexture* target = driver->newTexture(targetInfo);
        REQUIRE(target != nullptr);

        vkm::VkmBufferInfo lightBufferInfo{};
        lightBufferInfo._flags =
            vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
        lightBufferInfo._size = sizeof(vkm::VkmDeferredLightConstants);
        lightBufferInfo._debugName = "ShadowedLightConstants";
        vkm::VkmBuffer* lightBuffer = driver->newBuffer(lightBufferInfo);
        REQUIRE(lightBuffer != nullptr);

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

        // One run: fill the atlas, fill the G-buffer, shade. `shadowed` chooses whether the
        // light's tile assignment survives into the constants.
        const auto render = [&](bool shadowed) {
            std::vector<vkm::VkmPunctualLight> passLights = lights;
            if (!shadowed)
            {
                passLights[0]._shadowTile = -1;
            }
            vkm::VkmDeferredLightConstants lightConstants{};
            vkm::vkmBuildDeferredLightConstants(passLights, atlas.getTilesPerRow(),
                                                atlas.getDescriptor()._tileSize, &lightConstants);
            REQUIRE(driver->uploadToBuffer(lightBuffer->getHandle(), &lightConstants, sizeof(lightConstants)));

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

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);

            // The atlas first: the lookup below reads what this writes.
            atlas.record(&renderGraph, &scene, frameData, /*frameIndex=*/0);

            std::vector<vkm::VkmResourceAccessDeclaration> referenced;
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("CameraSceneUpdate");
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Update, &referenced);
            updateSubGraph->addReferencedResources(referenced);
            updateSubGraph->setTransferCallback([&scene, &frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });

            auto* cullSubGraph = renderGraph.beginComputeSubGraph("CameraSceneCull");
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

            vkm::VkmTextureReadbackResult readback = driver->readbackTexture(target->getHandle());
            table->destroy();
            delete table;
            return readback;
        };

        const vkm::VkmTextureReadbackResult shadowed = render(true);
        const vkm::VkmTextureReadbackResult unshadowed = render(false);
        REQUIRE(shadowed.channels == 8); // bytes per texel, RGBA16F

        // A world point's texel, by projecting it exactly as the rasterizer did. Clip space is
        // +Y up and the readback is top-left origin, hence the V flip.
        const auto texelFor = [&](float worldX, float worldZ) {
            const glm::vec4 clip = viewProjection * glm::vec4(worldX, 0.0f, worldZ, 1.0f);
            REQUIRE(clip.w > 0.0f);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            const uint32_t x = static_cast<uint32_t>((ndc.x * 0.5f + 0.5f) * kSize);
            const uint32_t y = static_cast<uint32_t>((0.5f - ndc.y * 0.5f) * kSize);
            return glm::uvec2(glm::min(x, kSize - 1u), glm::min(y, kSize - 1u));
        };
        const auto luminanceAt = [&](const vkm::VkmTextureReadbackResult& readback, glm::uvec2 texel) {
            const uint8_t* p = &readback.pixels[(static_cast<size_t>(texel.y) * readback.width + texel.x) *
                                                readback.channels];
            return readHalfComponent(p, 0) + readHalfComponent(p, 1) + readHalfComponent(p, 2);
        };

        // Floor deep inside the umbra (x in [-3, -1]), and floor directly beneath the light.
        // Both are the same material on the same plane, so nothing but visibility separates each
        // point's two renders.
        const glm::uvec2 umbra = texelFor(-2.0f, 0.0f);
        const glm::uvec2 lit = texelFor(3.0f, 0.0f);

        const float umbraShadowed = luminanceAt(shadowed, umbra);
        const float umbraUnshadowed = luminanceAt(unshadowed, umbra);
        const float litShadowed = luminanceAt(shadowed, lit);
        const float litUnshadowed = luminanceAt(unshadowed, lit);

        // The unshadowed run must actually light both points, or every ratio below is vacuous.
        REQUIRE(umbraUnshadowed > 0.0f);
        REQUIRE(litUnshadowed > 0.0f);

        // The assertion with teeth: with the shadow term on, the occluded point loses almost all
        // its light while a point on the same floor, lit by the same light through the same
        // BRDF, keeps its own. Only the visibility term distinguishes them, because the two runs
        // differ in nothing else.
        CHECK(umbraShadowed < umbraUnshadowed * 0.05f);
        CHECK(litShadowed == doctest::Approx(litUnshadowed).epsilon(0.02));

        // Acne: over a patch of floor known to be lit, the shadow term must not speckle. The
        // patch is defined in WORLD space and projected, not as a pixel neighbourhood: a pixel
        // box around a point drifts across the shadow boundary or off the floor entirely under a
        // perspective camera, and would then be measuring geometry rather than acne.
        // x in [2.4, 3.6], z in [-0.6, 0.6] is floor, outside the umbra's x in [-3, 0], and clear
        // of the occluder's own footprint.
        float patchMin = 1.0e9f;
        float patchMax = 0.0f;
        for (int i = 0; i <= 6; ++i)
        {
            for (int j = 0; j <= 6; ++j)
            {
                const float wx = 2.4f + 1.2f * (static_cast<float>(i) / 6.0f);
                const float wz = -0.6f + 1.2f * (static_cast<float>(j) / 6.0f);
                const float value = luminanceAt(shadowed, texelFor(wx, wz));
                patchMin = glm::min(patchMin, value);
                patchMax = glm::max(patchMax, value);
            }
        }
        // Every sample lit at all: a bias so small that the floor self-shadows shows up here as a
        // zero, and one so large that the umbra leaks shows up in the umbra check above. The two
        // bracket the bias from both sides, which is the thing normally tuned by eye.
        REQUIRE(patchMin > 0.0f);
        // The patch spans real distance from the light, so its brightness genuinely varies by
        // 1/d^2; what acne would add is speckle far beyond that gradient.
        CHECK(patchMax / patchMin < 1.6f);

        atlas.destroy();
        driver->getRenderResourcePool()->releaseResource(lightBuffer->getHandle());
        driver->getRenderResourcePool()->releaseResource(target->getHandle());
        driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
        gbuffer.destroy();
        scene.destroy(driver);
    }
} // namespace vkmtest

#endif // TEST_SHADOWED_LIGHTING_SHARED_HPP
