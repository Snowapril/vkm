#ifndef TEST_SHADOW_ATLAS_SHARED_HPP
#define TEST_SHADOW_ATLAS_SHARED_HPP

#include <doctest/doctest.h>

#include "TestHalfFloatShared.hpp"

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/probe_volume.h>
#include <vkm/renderer/shadow_atlas.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace vkmtest
{
    /*
    * @brief The atlas holds the true world distance from each light to what it sees.
    *
    * Geometry, not shading: this runs before anything consumes the atlas, so what it can prove is
    * that the tile rectangles, the per-tile view-projections and the stored distance convention
    * all agree. A wrong matrix, a wrong tile rect or a radial-versus-axial mix-up each shows up
    * here as a number, whereas in a shaded image all three look like "the shadows are off".
    *
    * The fixture is the unit triangle at z = 0 spanning x,y in [0,1], placed by identity so its
    * world position is known exactly.
    */
    inline void runShadowAtlasTest(vkm::VkmDriverBase* driver)
    {
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

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        REQUIRE_MESSAGE(scene.build(driver, &manager, &error), error);
        scene.setObjectTransform(0, glm::mat4(1.0f));

        // Set 1 is published for every graphics pipeline whether or not a shader reads it, so a
        // render test has to fill it even when the pass under test needs nothing from it.
        vkm::VkmFrameConstants frameConstants{};
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        vkm::VkmShadowAtlas atlas;
        vkm::VkmShadowAtlas::Descriptor descriptor;
        descriptor._tileSize = 64u;
        descriptor._cullViewIndex = 2u;
        std::string atlasError;
        REQUIRE_MESSAGE(atlas.initialize(driver, &manager, descriptor, &atlasError), atlasError);
        REQUIRE_MESSAGE(atlas.prepareScene(scene, &atlasError), atlasError);

        // A spot light straight above the triangle's centre, aimed down. Its distance to the
        // plane is exactly kHeight, and the tile's centre texel looks straight down the cone axis.
        constexpr float kHeight = 4.0f;
        std::vector<vkm::VkmPunctualLight> lights(1);
        vkm::VkmPunctualLight& spot = lights[0];
        spot._type = static_cast<uint32_t>(vkm::VkmLightType::Spot);
        spot._positionWorld[0] = 0.5f;
        spot._positionWorld[1] = 0.5f;
        spot._positionWorld[2] = kHeight;
        spot._directionWorld[0] = 0.0f;
        spot._directionWorld[1] = 0.0f;
        spot._directionWorld[2] = -1.0f;
        spot._range = 20.0f;
        spot._cosInner = std::cos(0.4f);
        spot._cosOuter = std::cos(0.6f);
        spot._radiance[0] = 1.0f;
        spot._radiance[1] = 1.0f;
        spot._radiance[2] = 1.0f;

        atlas.allocate(scene, &lights);
        // A spot owns exactly one tile, and allocate() must have told the light which.
        CHECK(atlas.getTileCount() == 1);
        CHECK(lights[0]._shadowTile == 0);

        vkm::VkmFrameData frameData;
        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        atlas.record(&renderGraph, &scene, frameData, /*frameIndex=*/0);
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(atlas.getAtlasTexture());
        REQUIRE(readback.channels == 8); // bytes per texel, RGBA16F
        const auto texelAt = [&](uint32_t x, uint32_t y) {
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };

        // Assert over the tile's whole contents rather than one texel. The triangle projects to
        // roughly 10x10 texels at the centre of a 64x64 tile, and the exact centre texel lands on
        // its hypotenuse -- so a single-texel probe is a coin flip on rasterization fill rules.
        // What is exactly known is the geometry: the fixture spans x,y in [0,1] at z = 0 and the
        // light sits at (0.5, 0.5, kHeight), so the nearest point of the triangle is directly
        // below the light at distance kHeight, and the farthest vertex is (1, 0, 0).
        const float expectedMin = kHeight;
        const float expectedMax = std::sqrt(0.5f * 0.5f + 0.5f * 0.5f + kHeight * kHeight);

        uint32_t written = 0;
        float minDistance = 1.0e9f;
        float maxDistance = 0.0f;
        for (uint32_t y = 0; y < descriptor._tileSize; ++y)
        {
            for (uint32_t x = 0; x < descriptor._tileSize; ++x)
            {
                const float value = readHalfComponent(texelAt(x, y), 0);
                if (value < 1000.0f)
                {
                    ++written;
                    minDistance = std::min(minDistance, value);
                    maxDistance = std::max(maxDistance, value);
                }
            }
        }

        // Something was drawn at all: this alone catches a wrong tile rect, a wrong matrix or a
        // stride mistake, each of which renders the tile empty rather than wrong.
        REQUIRE(written > 0);
        // And the distances are the true ones. Half-float's ulp at 4 is about 0.004, so 1% is
        // generous for the mantissa yet far tighter than any convention error: an axial-instead-
        // of-radial distance, a near/far mix-up or a light position off by the scene's offset all
        // land well outside.
        CHECK(minDistance == doctest::Approx(expectedMin).epsilon(0.01));
        CHECK(maxDistance <= expectedMax * 1.01f);
        CHECK(maxDistance >= expectedMin);

        // A tile nothing was drawn into keeps the clear value, which is what tells a lookup
        // "nothing occludes" rather than "an occluder sits at the light".
        const uint32_t emptyX = descriptor._tileSize + descriptor._tileSize / 2;
        const uint32_t emptyY = descriptor._tileSize / 2;
        CHECK(readHalfComponent(texelAt(emptyX, emptyY), 0) > 1000.0f);

        atlas.destroy();
        scene.destroy(driver);
    }

    /*
    * @brief A point light takes six tiles; a spot and a directional take one each.
    *
    * Tile accounting is what the lookup's face selection indexes into, so an off-by-one here
    * would silently make every point light read another light's tile.
    */
    inline void runShadowAtlasAllocationTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);

        vkm::VkmScene scene;
        vkm::VkmShadowAtlas atlas;
        vkm::VkmShadowAtlas::Descriptor descriptor;
        descriptor._tileSize = 32u;
        std::string atlasError;
        REQUIRE_MESSAGE(atlas.initialize(driver, &manager, descriptor, &atlasError), atlasError);

        const auto makeLight = [](vkm::VkmLightType type) {
            vkm::VkmPunctualLight light;
            light._type = static_cast<uint32_t>(type);
            light._range = 5.0f;
            light._cosInner = 0.9f;
            light._cosOuter = 0.8f;
            light._radiance[0] = 1.0f;
            return light;
        };

        std::vector<vkm::VkmPunctualLight> lights{
            makeLight(vkm::VkmLightType::Directional),
            makeLight(vkm::VkmLightType::Point),
            makeLight(vkm::VkmLightType::Spot),
        };
        atlas.allocate(scene, &lights);

        CHECK(lights[0]._shadowTile == 0); // directional: one tile
        CHECK(lights[1]._shadowTile == 1); // point: six, starting here
        CHECK(lights[2]._shadowTile == 7); // spot: the one after them
        CHECK(atlas.getTileCount() == 8);

        // Past the budget a light keeps shading and simply casts no shadow -- a dropped light
        // would be a hole in the image, an unshadowed one only a missing shadow.
        std::vector<vkm::VkmPunctualLight> many(vkm::kVkmMaxShadowTiles + 4,
                                                makeLight(vkm::VkmLightType::Spot));
        atlas.allocate(scene, &many);
        CHECK(atlas.getTileCount() == vkm::kVkmMaxShadowTiles);
        CHECK(many[vkm::kVkmMaxShadowTiles - 1]._shadowTile >= 0);
        CHECK(many[vkm::kVkmMaxShadowTiles]._shadowTile == -1);
        CHECK(many.back()._shadowTile == -1);

        atlas.destroy();
    }
    /*
    * @brief The probe capture consults the shadow atlas, so a probe under an occluder records the
    * floor as shadowed rather than sunlit.
    *
    * This is the term that decides whether the probe tier over-reports indoors: a probe that
    * captures a wall as sunlit hands that brightness to every surface around it, whether or not
    * the sun can actually reach the wall.
    *
    * Same design as the deferred gate -- the capture runs twice with everything identical except
    * the sun's `_shadowTile`, so albedo, nDotL and the sun's radiance cannot explain a difference.
    * The fixture's floor is at y = 0 and its occluder spans +-1 at y = 2 under a sun straight
    * overhead, so the floor directly below the occluder is fully shadowed and the floor beyond
    * +-1 is fully lit.
    */
    inline void runShadowedProbeCaptureTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kFaceSize = 32;
        constexpr uint32_t kFacesX = 3;
        constexpr uint32_t kFacesY = 2;

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

        const std::string psoName =
            std::string("probe_capture_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState(psoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        // Sun straight down, so the occluder's shadow lands directly beneath it.
        vkm::VkmFrameData frameData;
        frameData._lightDirection = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);

        std::vector<vkm::VkmPunctualLight> lights(1);
        vkm::VkmPunctualLight& sun = lights[0];
        sun._type = static_cast<uint32_t>(vkm::VkmLightType::Directional);
        sun._directionWorld[0] = 0.0f;
        sun._directionWorld[1] = -1.0f;
        sun._directionWorld[2] = 0.0f;
        sun._radiance[0] = 1.0f;
        sun._radiance[1] = 1.0f;
        sun._radiance[2] = 1.0f;

        vkm::VkmShadowAtlas atlas;
        vkm::VkmShadowAtlas::Descriptor atlasDescriptor;
        atlasDescriptor._tileSize = 512u;
        atlasDescriptor._cullViewIndex = 2u;
        std::string atlasError;
        REQUIRE_MESSAGE(atlas.initialize(driver, &manager, atlasDescriptor, &atlasError), atlasError);
        REQUIRE_MESSAGE(atlas.prepareScene(scene, &atlasError), atlasError);
        atlas.allocate(scene, &lights);
        REQUIRE(lights[0]._shadowTile == 0);

        vkm::VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "ProbeShadowSampler";
        vkm::VkmSampler* sampler = driver->newSampler(samplerInfo);
        REQUIRE(sampler != nullptr);

        const glm::uvec2 captureExtent(kFaceSize * kFacesX, kFaceSize * kFacesY);
        const auto makeTarget = [&](vkm::VkmFormat format, vkm::VkmResourceCreateInfo flags, const char* name) {
            vkm::VkmTextureInfo info{};
            info._flags = flags;
            info._extent = glm::uvec3(captureExtent, 1);
            info._numMipLevels = 1;
            info._numArrayLayers = 1;
            info._format = format;
            info._debugName = name;
            vkm::VkmTexture* texture = driver->newTexture(info);
            REQUIRE(texture != nullptr);
            return texture;
        };
        vkm::VkmTexture* captureTarget = makeTarget(
            vkm::VkmFormat::R16G16B16A16_SFLOAT,
            static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowColorAttachment) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferSrc)),
            "ProbeShadowCapture");
        vkm::VkmTexture* captureDepth =
            makeTarget(vkm::VkmFormat::D32_SFLOAT, vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment,
                       "ProbeShadowCaptureDepth");

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = captureExtent.x;
        fbDesc._height = captureExtent.y;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._colorAttachments[0] = captureTarget->getHandle();
        vkm::VkmDepthStencilAttachmentDescriptor depthDesc{};
        depthDesc._attachmentId = 1;
        depthDesc._loadAction = vkm::VkmLoadAction::Clear;
        depthDesc._storeAction = vkm::VkmStoreAction::Store;
        depthDesc._clearDepth = 1.0f;
        fbDesc._renderPass._depthStencilAttachment = depthDesc;
        fbDesc._depthStencilAttachment = captureDepth->getHandle();

        vkm::VkmBufferInfo captureBufferInfo{};
        captureBufferInfo._flags =
            vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
        captureBufferInfo._size = sizeof(vkm::VkmProbeCaptureConstants);
        captureBufferInfo._debugName = "ProbeShadowCaptureConstants";
        vkm::VkmBuffer* captureBuffer = driver->newBuffer(captureBufferInfo);
        REQUIRE(captureBuffer != nullptr);

        std::string tableError;
        vkm::VkmResourceTableBase* table = driver->newResourceTable(
            pso, vkm::VkmResourceSetKind::PerPass,
            {{ 0, captureBuffer->getHandle() },
             { 1, atlas.getAtlasTexture() },
             { 2, sampler->getHandle() },
             { 3, atlas.getConstantBuffer() }},
            &tableError);
        REQUIRE_MESSAGE(table != nullptr, tableError);

        // Between the floor and the occluder, so face 3 (-Y) looks straight down at floor the
        // occluder is shading.
        const glm::vec3 probePosition(0.0f, 1.0f, 0.0f);

        const auto capture = [&](bool shadowed) {
            vkm::VkmProbeCaptureConstants constants{};
            vkm::vkmBuildProbeFaceViewProjections(glm::vec3(0.0f), 0.05f, 100.0f,
                                                  constants._faceViewProjection);
            constants._sun = lights[0];
            if (!shadowed)
            {
                constants._sun._shadowTile = -1;
            }
            constants._shadowParams =
                glm::uvec4(atlas.getTilesPerRow(), atlas.getDescriptor()._tileSize, 0u, 0u);
            REQUIRE(driver->uploadToBuffer(captureBuffer->getHandle(), &constants, sizeof(constants)));

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            atlas.record(&renderGraph, &scene, frameData, /*frameIndex=*/0);

            std::vector<vkm::VkmResourceAccessDeclaration> referenced;
            auto* updateSubGraph = renderGraph.beginTransferSubGraph("ProbeShadowSceneUpdate");
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Update, &referenced);
            updateSubGraph->addReferencedResources(referenced);
            updateSubGraph->setTransferCallback([&scene, &frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });

            auto* cullSubGraph = renderGraph.beginComputeSubGraph("ProbeShadowSceneCull");
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Cull, &referenced);
            cullSubGraph->addReferencedResources(referenced);
            cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordCull(commandBuffer, /*viewIndex=*/0);
            });

            auto* captureSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
            referenced.clear();
            scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
            captureSubGraph->addReferencedResources(referenced);
            std::vector<vkm::VkmResourceAccessDeclaration> bound;
            table->collectReferencedResources(&bound);
            captureSubGraph->addReferencedResources(bound);
            captureSubGraph->setRenderCallback([&scene, pso, table, probePosition](vkm::VkmCommandBufferBase* commandBuffer) {
                for (uint32_t face = 0; face < 6; ++face)
                {
                    commandBuffer->setViewportAndScissor(
                        static_cast<int32_t>((face % kFacesX) * kFaceSize),
                        static_cast<int32_t>((face / kFacesX) * kFaceSize), kFaceSize, kFaceSize);

                    vkm::VkmProbeCapturePushConstants push{};
                    push._probePositionWorld = probePosition;
                    push._faceIndex = face;

                    scene.recordDrawBatches(
                        commandBuffer, [pso](const vkm::VkmScene::DrawBatch&) { return pso; },
                        [table, &push](vkm::VkmCommandBufferBase* cb, const vkm::VkmScene::DrawBatch&) {
                            cb->bindResourceTable(table);
                            cb->setPushConstants(&push, sizeof(push));
                        },
                        /*viewIndex=*/0);
                }
            });

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
            return driver->readbackTexture(captureTarget->getHandle());
        };

        const vkm::VkmTextureReadbackResult shadowed = capture(true);
        const vkm::VkmTextureReadbackResult unshadowed = capture(false);
        REQUIRE(shadowed.channels == 8);

        // Face 3 is -Y, straight down at the shadowed floor.
        const uint32_t faceX = (3u % kFacesX) * kFaceSize + kFaceSize / 2;
        const uint32_t faceY = (3u / kFacesX) * kFaceSize + kFaceSize / 2;
        const auto radianceAt = [&](const vkm::VkmTextureReadbackResult& readback) {
            const uint8_t* p =
                &readback.pixels[(static_cast<size_t>(faceY) * readback.width + faceX) * readback.channels];
            return readHalfComponent(p, 0) + readHalfComponent(p, 1) + readHalfComponent(p, 2);
        };

        const float litRadiance = radianceAt(unshadowed);
        const float shadowedRadiance = radianceAt(shadowed);

        // Without this the ratio below is vacuous: the probe must actually see lit floor first.
        REQUIRE(litRadiance > 0.0f);
        // And with the occluder's tile assigned, the floor it captures goes dark. Only the
        // visibility term differs between the two captures.
        CHECK(shadowedRadiance < litRadiance * 0.05f);

        table->destroy();
        delete table;
        atlas.destroy();
        driver->getRenderResourcePool()->releaseResource(captureBuffer->getHandle());
        driver->getRenderResourcePool()->releaseResource(captureDepth->getHandle());
        driver->getRenderResourcePool()->releaseResource(captureTarget->getHandle());
        driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
        scene.destroy(driver);
    }
} // namespace vkmtest

#endif // TEST_SHADOW_ATLAS_SHARED_HPP
