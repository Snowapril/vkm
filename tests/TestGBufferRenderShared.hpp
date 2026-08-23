#ifndef TEST_GBUFFER_RENDER_SHARED_HPP
#define TEST_GBUFFER_RENDER_SHARED_HPP

#include <doctest/doctest.h>

#include "TestHalfFloatShared.hpp"

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture.h>
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
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <chrono>
#include <cstdio>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

/*
* Renders the fixture triangle through the real G-buffer PSO and reads every channel back.
*
* This is what proves the pieces agree end to end: gbuffer.hlsl's MRT writes, vkm_gbuffer.hlsli's
* octahedral packing, VkmGBuffer's formats and attachment order, and the motion-vector maths
* against the frame constants' prevViewProjection. Each channel is checked against a value that
* could not appear by accident -- the fixture's material colour, a known facing direction, a
* motion vector that must be exactly zero -- rather than against "something was written".
*/

namespace vkmtest
{
    constexpr uint32_t kGBufferRenderSize = 64;

    // The fixture material is (0.25, 0.5, 0.75, 1.0); see resources/tests/gltf_triangle.gltf.
    constexpr float kFixtureBaseColorR = 0.25f;
    constexpr float kFixtureBaseColorG = 0.5f;
    constexpr float kFixtureBaseColorB = 0.75f;

    /*
    * @brief Imports the fixture triangle, builds a scene, and rasterizes it into `gbuffer`.
    *
    * Shared by the G-buffer channel checks and the deferred-lighting test, which needs a filled
    * G-buffer to read. Returns the view-projection used, since a consumer reconstructing world
    * positions needs the same one.
    */
    inline void recordGBufferFrame(vkm::VkmDriverBase* driver,
                                   vkm::VkmScene& scene,
                                   vkm::VkmGBuffer& gbuffer,
                                   vkm::VkmPipelineStateBase* pso,
                                   const vkm::VkmFrameData& frameData);

    inline glm::mat4 fillGBuffer(vkm::VkmDriverBase* driver,
                                 vkm::VkmPipelineStateManager& manager,
                                 vkm::VkmScene& scene,
                                 vkm::VkmGBuffer& gbuffer,
                                 const char* gltfName = "tests/gltf_triangle.gltf")
    {
        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE(vkm::importGltfModel(std::string(RESOURCES_DIR) + gltfName, &model, &error, importOptions));

        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR, TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);

        REQUIRE(scene.addModel(model, &error));
        REQUIRE(scene.build(driver, &manager, &error));

        const std::string psoName =
            std::string("gbuffer_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState(psoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        REQUIRE(gbuffer.initialize(driver, glm::uvec2(kGBufferRenderSize, kGBufferRenderSize)));

        // Same placement as the scene-model render test: the fixture triangle spans x,y in [0,1]
        // at z = 0, so this maps it onto the lower-left half of clip space, front-facing under the
        // PSO's back-face culling.
        scene.setObjectTransform(0, glm::mat4(1.0f));
        const glm::mat4 viewProjection = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.5f)) *
                                         glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));

        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);
        frameData._lightDirection = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);

        vkm::VkmFrameConstants frameConstants{};
        frameConstants._viewProjection = viewProjection;
        // The deferred lighting pass reconstructs world positions through this, so it has to be
        // the real inverse rather than the default identity.
        frameConstants._inverseViewProjection = glm::inverse(viewProjection);
        // A still camera: the motion vectors must come out exactly zero, which is a far stronger
        // statement than "small". Any sign error, Y-flip mistake or stale matrix breaks it.
        // Motion is computed from the jitter-free pair, and this fixture carries no jitter, so
        // both matrices equal viewProjection.
        frameConstants._viewProjectionNoJitter = viewProjection;
        frameConstants._prevViewProjection = viewProjection;
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        recordGBufferFrame(driver, scene, gbuffer, pso, frameData);

        return viewProjection;
    }

    /*
    * @brief Records and runs one G-buffer frame over an already-built scene.
    *
    * Split out of fillGBuffer so a test can render the same scene twice -- which is what checking
    * a texture the streamer replaced between the two needs.
    */
    inline void recordGBufferFrame(vkm::VkmDriverBase* driver,
                                   vkm::VkmScene& scene,
                                   vkm::VkmGBuffer& gbuffer,
                                   vkm::VkmPipelineStateBase* pso,
                                   const vkm::VkmFrameData& frameData)
    {
        std::vector<vkm::VkmResourceAccessDeclaration> referenced;

        const vkm::VkmFrameBufferDescriptor fbDesc = gbuffer.makeFrameBufferDescriptor();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* updateSubGraph = renderGraph.beginTransferSubGraph("SceneUpdate");
        referenced.clear();
        scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Update, &referenced);
        updateSubGraph->addReferencedResources(referenced);
        updateSubGraph->setTransferCallback([&scene, &frameData](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData);
        });

        auto* cullSubGraph = renderGraph.beginComputeSubGraph("SceneCull");
        referenced.clear();
        scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Cull, &referenced);
        cullSubGraph->addReferencedResources(referenced);
        cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordCull(commandBuffer);
        });

        auto* drawSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        referenced.clear();
        scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
        drawSubGraph->addReferencedResources(referenced);
        drawSubGraph->setRenderCallback([&scene, pso](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordDrawBatches(commandBuffer, [pso](const vkm::VkmScene::DrawBatch&) { return pso; });
        });

        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();
    }

    inline void runGBufferRenderTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmScene scene;
        vkm::VkmGBuffer gbuffer;
        fillGBuffer(driver, manager, scene, gbuffer);


        // A pixel well inside the covered lower-left half, matching the scene-model test's choice.
        const uint32_t sampleX = kGBufferRenderSize / 4;
        const uint32_t sampleY = kGBufferRenderSize * 3 / 4;

        SUBCASE("base colour and roughness carry the material factors")
        {
            const vkm::VkmTextureReadbackResult readback =
                driver->readbackTexture(gbuffer.getTexture(vkm::VkmGBuffer::Target::BaseColorRoughness));
            REQUIRE(readback.channels == 4);
            const uint8_t* texel =
                &readback.pixels[(static_cast<size_t>(sampleY) * readback.width + sampleX) * readback.channels];

            // RGBA8_UNORM, so the material's floats round-trip to within half a step.
            CHECK(texel[0] / 255.0f == doctest::Approx(kFixtureBaseColorR).epsilon(0.01));
            CHECK(texel[1] / 255.0f == doctest::Approx(kFixtureBaseColorG).epsilon(0.01));
            CHECK(texel[2] / 255.0f == doctest::Approx(kFixtureBaseColorB).epsilon(0.01));
        }

        SUBCASE("the emissive target carries factor times strength")
        {
            // The fixture's emissiveFactor (0.1, 0.2, 0.3) times its
            // KHR_materials_emissive_strength of 5 -- which is also what proves the strength
            // survived import, since the raw factor alone cannot exceed 1.
            const vkm::VkmTextureReadbackResult readback =
                driver->readbackTexture(gbuffer.getTexture(vkm::VkmGBuffer::Target::Emissive));
            REQUIRE(readback.channels == 8);
            const uint8_t* texel =
                &readback.pixels[(static_cast<size_t>(sampleY) * readback.width + sampleX) * readback.channels];

            CHECK(readHalfComponent(texel, 0) == doctest::Approx(0.5f).epsilon(0.01));
            CHECK(readHalfComponent(texel, 1) == doctest::Approx(1.0f).epsilon(0.01));
            CHECK(readHalfComponent(texel, 2) == doctest::Approx(1.5f).epsilon(0.01));
        }

        SUBCASE("a still camera produces exactly zero motion")
        {
            // VkmTextureReadbackResult::channels is bytes per texel, not a channel count -- the
            // two only coincide for 8-bit RGBA (see readbackTexture). This target is RGBA16F, so
            // it reports 8.
            const vkm::VkmTextureReadbackResult readback =
                driver->readbackTexture(gbuffer.getTexture(vkm::VkmGBuffer::Target::MotionMetallic));
            REQUIRE(readback.channels == 8);
            const uint8_t* texel =
                &readback.pixels[(static_cast<size_t>(sampleY) * readback.width + sampleX) * readback.channels];

            // Decoded rather than compared as raw bits: the Y term is negated, so a zero motion
            // legitimately arrives as *negative* zero (0x8000), which is not all-zero bits but is
            // equal to 0.0f.
            CHECK(readHalfComponent(texel, 0) == 0.0f);
            CHECK(readHalfComponent(texel, 1) == 0.0f);
        }

        // The scene's buffers outlive this body otherwise, and VMA reports them as unfreed when
        // the allocator goes away -- after which the resource pool's own destructor runs their
        // destructors against a destroyed allocator. Metal tolerated the leak; Vulkan segfaults.
        driver->waitIdle();
        gbuffer.destroy();
        scene.destroy(driver);
        driver->getDeferredReclaimer()->flushBlocking();
    }

    /*
    * @brief The material's base-colour *texture* reaches the G-buffer, not just its factor.
    *
    * @details resources/tests/gltf_textured.gltf pairs a baseColorFactor of (0.25, 0.5, 0.75) with
    * a solid green (0, 255, 0) baseColorTexture, and glTF multiplies the two -- so the shaded
    * result is (0, 0.5, 0). Every channel discriminates: red and blue collapse to zero only if the
    * texture was sampled, and green survives only if the factor was still applied. A material that
    * ignored its texture would read back the factor itself.
    *
    * The image is uploaded as R8G8B8A8_SRGB, which is what makes a fully-saturated 255 decode to
    * linear 1.0 and leave the factor unscaled.
    */
    inline void runMaterialTextureTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmScene scene;
        vkm::VkmGBuffer gbuffer;
        fillGBuffer(driver, manager, scene, gbuffer, "tests/gltf_textured.gltf");

        const uint32_t sampleX = kGBufferRenderSize / 4;
        const uint32_t sampleY = kGBufferRenderSize * 3 / 4;

        const vkm::VkmTextureReadbackResult readback =
            driver->readbackTexture(gbuffer.getTexture(vkm::VkmGBuffer::Target::BaseColorRoughness));
        REQUIRE(readback.channels == 4);
        const uint8_t* texel =
            &readback.pixels[(static_cast<size_t>(sampleY) * readback.width + sampleX) * readback.channels];

        CHECK(texel[0] / 255.0f == doctest::Approx(0.0f).epsilon(0.02));
        CHECK(texel[1] / 255.0f == doctest::Approx(kFixtureBaseColorG).epsilon(0.02));
        CHECK(texel[2] / 255.0f == doctest::Approx(0.0f).epsilon(0.02));

        // Stated separately so a regression reads as "the factor came through untextured" rather
        // than as three unrelated channel failures.
        CHECK(texel[0] / 255.0f < kFixtureBaseColorR * 0.5f);
        CHECK(texel[2] / 255.0f < kFixtureBaseColorB * 0.5f);

        gbuffer.destroy();
        scene.destroy(driver);
    }

    /*
    * @brief A streamed-out material texture is a different resource, and still samples correctly.
    *
    * @details The streamer answers a distant camera by building a *new*, smaller texture holding
    * only the levels it needs and re-pointing the material at it -- not by narrowing a view. Two
    * things have to hold afterwards, and neither implies the other:
    *
    *   - the resident base level actually moved, which is the only externally visible proof the
    *     rebuild happened at all (a chain of a solid colour renders identically at every level, so
    *     pixels alone cannot tell), and
    *   - the material still samples green, which is what proves the new bindless slot reached the
    *     material record. A swap that rebuilt the texture but left the record naming the released
    *     slot would sample whatever took that slot over, or nothing.
    *
    * The camera is placed far enough out that the fixture's 64x64 texture cannot justify level 0,
    * and the settings are wound down so the convergence this measures is correctness, not pacing.
    */
    inline void runTextureStreamingSwapTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmScene scene;
        vkm::VkmGBuffer gbuffer;
        fillGBuffer(driver, manager, scene, gbuffer, "tests/gltf_textured.gltf");

        if (!scene.isTextureStreamingAvailable())
        {
            MESSAGE("Skipping: this backend has no bindless texture array, so nothing streams.");
            gbuffer.destroy();
            scene.destroy(driver);
            return;
        }

        REQUIRE(scene.getStreamedBaseMip(/*materialIndex=*/0, /*channel=*/0) == 0);

        vkm::VkmTextureStreamingSettings settings;
        settings._stableTickCount = 0;      // act on the first tick that asks
        settings._maxLevelUploadsPerTick = 64; // and finish the rebuild in that same tick
        scene.setTextureStreamingSettings(settings);

        vkm::VkmTextureStreamingView view;
        view._viewportHeight = kGBufferRenderSize;
        view._fovYRadians = 0.8726646f;
        // Far enough that a 64x64 texture over a unit-ish object is well past one texel per pixel.
        view._cameraPosition = glm::vec3(0.0f, 0.0f, 4096.0f);

        // The decode runs on the streamer's worker, so the level moves some ticks after the first
        // one asks. Bounded rather than open-ended: a streamer that never converges must fail here
        // rather than hang the suite.
        uint32_t streamedBaseMip = 0;
        for (uint32_t tick = 0; tick < 256 && streamedBaseMip == 0; ++tick)
        {
            scene.updateTextureStreaming(driver, view);
            streamedBaseMip = scene.getStreamedBaseMip(/*materialIndex=*/0, /*channel=*/0);
            if (streamedBaseMip == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        CHECK(streamedBaseMip > 0);

        // Re-render through whatever the material now names.
        const std::string psoName =
            std::string("gbuffer_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState(psoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        vkm::VkmFrameData frameData;
        const glm::mat4 viewProjection = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.5f)) *
                                         glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);
        frameData._lightDirection = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
        recordGBufferFrame(driver, scene, gbuffer, pso, frameData);

        const uint32_t sampleX = kGBufferRenderSize / 4;
        const uint32_t sampleY = kGBufferRenderSize * 3 / 4;
        const vkm::VkmTextureReadbackResult readback =
            driver->readbackTexture(gbuffer.getTexture(vkm::VkmGBuffer::Target::BaseColorRoughness));
        REQUIRE(readback.channels == 4);
        const uint8_t* texel =
            &readback.pixels[(static_cast<size_t>(sampleY) * readback.width + sampleX) * readback.channels];

        // Every level of a solid-green chain is solid green, so the same expectation the untouched
        // material meets is the right one here -- what changed is which resource produced it.
        CHECK(texel[0] / 255.0f == doctest::Approx(0.0f).epsilon(0.02));
        CHECK(texel[1] / 255.0f == doctest::Approx(kFixtureBaseColorG).epsilon(0.02));
        CHECK(texel[2] / 255.0f == doctest::Approx(0.0f).epsilon(0.02));

        gbuffer.destroy();
        scene.destroy(driver);
    }

    /*
    * @brief The GPU feedback loop, end to end: the shader writes, the ring carries it back, the
    * streamer acts on it.
    *
    * @details Everything between the pixel shader and `selectTargets` is machinery no pure-logic
    * test can reach -- the LOD query surviving the shader toolchain, the atomic landing in the right
    * bindless slot, the singleton being bound where the shader declared it, the readback ring's
    * index arithmetic, and the relative-to-absolute decode. A silent failure anywhere in that chain
    * looks exactly like "streaming is a bit conservative", which is why it needs asserting rather
    * than eyeballing.
    *
    * What it does not check is whether the reported level is *numerically* right for a given
    * geometry -- that needs a known UV parameterisation and is left to the Sponza A/B.
    */
    inline void runTextureFeedbackTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmScene scene;
        vkm::VkmGBuffer gbuffer;
        fillGBuffer(driver, manager, scene, gbuffer, "tests/gltf_textured.gltf");

        if (!scene.isTextureStreamingAvailable())
        {
            MESSAGE("Skipping: this backend has no bindless texture array, so nothing streams.");
            gbuffer.destroy();
            scene.destroy(driver);
            return;
        }

        // Nothing should have voted before a frame has been drawn.
        REQUIRE(scene.getTextureStreamingStats()._feedbackCount == 0);

        const std::string psoName =
            std::string("gbuffer_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState(psoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        vkm::VkmFrameData frameData;
        const glm::mat4 viewProjection = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.5f)) *
                                         glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);
        frameData._lightDirection = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);

        // A camera close enough that the estimate alone would keep everything at level 0, so a
        // target that ends up anywhere else came from the GPU rather than from the sphere.
        vkm::VkmTextureStreamingView view;
        view._viewportHeight = kGBufferRenderSize;
        view._fovYRadians = 0.8726646f;
        view._cameraPosition = glm::vec3(0.0f, 0.0f, 1.0f);

        /*
        * The reading is a whole ring behind by design, so a handful of frames have to go by before
        * any of it comes back. Bounded rather than open-ended: a loop that never delivers must fail
        * here rather than spin.
        */
        uint32_t feedbackCount = 0;
        for (uint32_t frame = 0; frame < 32 && feedbackCount == 0; ++frame)
        {
            scene.updateTextureStreaming(driver, view);
            recordGBufferFrame(driver, scene, gbuffer, pso, frameData);
            feedbackCount = scene.getTextureStreamingStats()._feedbackCount;
        }

        // The fixture's material samples one texture, so exactly one slot can have voted.
        CHECK(feedbackCount > 0);

        /*
        * And the loop must settle rather than walk the texture a level further every frame. Once
        * the texture holds what the shader asked for, the reading names that same level again and
        * nothing more should move -- so what is asserted is that the level stops changing, not what
        * it stops at. The level itself is this fixture's own measurement (see the note above about
        * numerical correctness) and is reported rather than pinned.
        */
        /*
        * A fixed warm-up before the level is read at all, because "unchanged" and "not started" look
        * identical from outside and the tiers differ in exactly that. Unbinding a level lands the
        * whole move in one tick; rebuilding waits out the streamer's damping, then a decode on the
        * worker, then a bounded number of uploads per tick. Sampling stability before all of that
        * has had room to happen reports the level the texture began at and calls it settled.
        */
        for (uint32_t frame = 0; frame < 64; ++frame)
        {
            scene.updateTextureStreaming(driver, view);
            recordGBufferFrame(driver, scene, gbuffer, pso, frameData);
        }
        const uint32_t settled = scene.getStreamedBaseMip(/*materialIndex=*/0, /*channel=*/0);
        CHECK(settled != vkm::INVALID_VALUE32);
        MESSAGE("Texture feedback settled this fixture at base mip " << settled);

        // Settled means it stays there: once the texture holds what the shader asked for, the
        // reading names that same level again and nothing more moves.
        for (uint32_t frame = 0; frame < 8; ++frame)
        {
            scene.updateTextureStreaming(driver, view);
            recordGBufferFrame(driver, scene, gbuffer, pso, frameData);
            CHECK(scene.getStreamedBaseMip(/*materialIndex=*/0, /*channel=*/0) == settled);
        }
        // A runaway loop ends at the coarsest level the chain has; a settled one never gets there.
        // The fixture's base colour image is 64x64, so its chain is 7 levels and 6 is the last.
        CHECK(settled < 6u);

        gbuffer.destroy();
        scene.destroy(driver);
    }

    /*
    * @brief Fills the G-buffer, hands it to the fullscreen lighting pass through descriptor set 2,
    * and checks the shaded result.
    *
    * Several threads meet here, which is the point of testing them together:
    *   - set 2 carrying sampled textures, a sampler and a uniform buffer (the buffer-only test
    *     never exercised the texture or sampler paths, and their Metal indices are pinned by
    *     vkm-compiler from the same declaration the runtime binds against),
    *   - the render graph handing an attachment over to be sampled, checked by pixels rather than
    *     by a tracked layout, and
    *   - the fullscreen triangle and PBR evaluation themselves.
    */
    inline void runDeferredLightingTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmScene scene;
        vkm::VkmGBuffer gbuffer;
        // The lighting pass reconstructs world positions from set 1's inverseViewProjection,
        // which the fill already published, so the returned matrix is not needed again here.
        fillGBuffer(driver, manager, scene, gbuffer);

        vkm::VkmPipelineStateBase* lightingPso =
            manager.getPipelineState("deferred_lighting_pso", vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(lightingPso != nullptr);

        vkm::VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "DeferredLightingSampler";
        vkm::VkmSampler* sampler = driver->newSampler(samplerInfo);
        REQUIRE(sampler != nullptr);

        struct LightConstants
        {
            float directionToLight[4];
            float radiance[4];
        };

        // Head-on with the fixture's +Z normals, so nDotL saturates and the material's colour
        // reaches the output as strongly as it can.
        const auto makeLightBuffer = [&](float intensity) {
            vkm::VkmBufferInfo info{};
            info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
            info._size = sizeof(LightConstants);
            info._debugName = "DeferredLightConstants";
            vkm::VkmBuffer* buffer = driver->newBuffer(info);
            REQUIRE(buffer != nullptr);
            const LightConstants constants{{0.0f, 0.0f, 1.0f, 0.0f},
                                           {intensity, intensity, intensity, 0.0f}};
            REQUIRE(driver->uploadToBuffer(buffer->getHandle(), &constants, sizeof(constants)));
            return buffer;
        };

        vkm::VkmTextureInfo lightingTargetInfo{};
        lightingTargetInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                                    vkm::VkmResourceCreateInfo::AllowTransferSrc;
        lightingTargetInfo._extent = glm::uvec3(kGBufferRenderSize, kGBufferRenderSize, 1);
        lightingTargetInfo._numMipLevels = 1;
        lightingTargetInfo._numArrayLayers = 1;
        // Matches the PSO's declared colour attachment; HDR because a lighting result is not
        // bounded to [0,1] before tone mapping.
        lightingTargetInfo._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        lightingTargetInfo._debugName = "DeferredLightingTarget";
        vkm::VkmTexture* lightingTarget = driver->newTexture(lightingTargetInfo);
        REQUIRE(lightingTarget != nullptr);

        vkm::VkmFrameBufferDescriptor lightingFb{};
        lightingFb._width = kGBufferRenderSize;
        lightingFb._height = kGBufferRenderSize;
        lightingFb._renderPass._colorAttachmentCount = 1;
        lightingFb._renderPass._colorAttachments[0]._attachmentId = 0;
        lightingFb._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        lightingFb._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        lightingFb._colorAttachments[0] = lightingTarget->getHandle();

        // Renders the lighting pass with `intensity` and returns the readback. A table is
        // immutable, so a different light means a different table -- which is exactly the usage
        // the immutability was designed around.
        const auto shadeWith = [&](float intensity) {
            vkm::VkmBuffer* lightBuffer = makeLightBuffer(intensity);
            const std::vector<vkm::VkmTableResourceEntry> entries{
                { 0, gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal) },
                { 1, gbuffer.getTexture(vkm::VkmGBuffer::Target::BaseColorRoughness) },
                { 2, gbuffer.getTexture(vkm::VkmGBuffer::Target::MotionMetallic) },
                { 3, sampler->getHandle() },
                { 4, lightBuffer->getHandle() },
                { 5, gbuffer.getTexture(vkm::VkmGBuffer::Target::Emissive) },
            };
            std::string tableError;
            vkm::VkmResourceTableBase* table =
                driver->newResourceTable(lightingPso, vkm::VkmResourceSetKind::PerPass, entries, &tableError);
            REQUIRE_MESSAGE(table != nullptr, tableError);

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* lightingSubGraph = renderGraph.beginGraphicsSubGraph(lightingFb);
            // Bound through the resource table, which the render graph cannot see -- so the
            // sampling has to be declared or nothing hands the attachments over. Colour targets
            // only: the depth attachment is never sampled (see gbuffer.h).
            for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
            {
                lightingSubGraph->addReferencedResource(gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)),
                                                        vkm::VkmResourceAccess::ShaderSampledRead);
            }
            lightingSubGraph->setRenderCallback([lightingPso, table](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(lightingPso);
                commandBuffer->bindResourceTable(table);
                commandBuffer->draw(3, 1, 0, 0); // one oversized triangle, no vertex buffer
            });

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            vkm::VkmTextureReadbackResult readback = driver->readbackTexture(lightingTarget->getHandle());
            table->destroy();
            delete table;
            driver->getRenderResourcePool()->releaseResource(lightBuffer->getHandle());
            return readback;
        };

        const uint32_t litX = kGBufferRenderSize / 4;
        const uint32_t litY = kGBufferRenderSize * 3 / 4;
        // The opposite corner, outside the lower-left half the triangle covers.
        const uint32_t bgX = kGBufferRenderSize - 4;
        const uint32_t bgY = 3;

        const vkm::VkmTextureReadbackResult single = shadeWith(1.0f);
        REQUIRE(single.channels == 8); // bytes per texel, RGBA16F
        const auto texelAt = [&](const vkm::VkmTextureReadbackResult& readback, uint32_t x, uint32_t y) {
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };

        const float litR = readHalfComponent(texelAt(single, litX, litY), 0);
        const float litG = readHalfComponent(texelAt(single, litX, litY), 1);
        const float litB = readHalfComponent(texelAt(single, litX, litY), 2);

        // Covered pixels are lit at all. If set 2 delivered nothing, every sample would read zero
        // and this would be black.
        CHECK(litR > 0.0f);
        CHECK(litG > 0.0f);
        CHECK(litB > 0.0f);

        // The material is (0.25, 0.5, 0.75), and a mostly-dielectric surface keeps that ordering
        // through the diffuse lobe. A shuffled or wrongly-bound base-colour texture breaks it.
        CHECK(litR < litG);
        CHECK(litG < litB);

        // Never covered by geometry, so the depth early-out must reject it outright.
        CHECK(readHalfComponent(texelAt(single, bgX, bgY), 0) == 0.0f);
        CHECK(readHalfComponent(texelAt(single, bgX, bgY), 1) == 0.0f);
        CHECK(readHalfComponent(texelAt(single, bgX, bgY), 2) == 0.0f);

        // Doubling only the set-2 uniform buffer doubles the LIT term. The output is
        // lighting + emission, and the fixture emits (0.5, 1.0, 1.5) -- factor times its
        // emissive strength -- so the emissive summand is subtracted before asserting
        // linearity in the light; asserting on the raw sum would demand the emitter brighten
        // with a light it does not reflect.
        const vkm::VkmTextureReadbackResult doubled = shadeWith(2.0f);
        const float kEmissive[3] = { 0.5f, 1.0f, 1.5f };
        const float lit[3] = { litR, litG, litB };
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            CHECK(readHalfComponent(texelAt(doubled, litX, litY), channel) - kEmissive[channel] ==
                  doctest::Approx((lit[channel] - kEmissive[channel]) * 2.0f).epsilon(0.02));
        }

        driver->getRenderResourcePool()->releaseResource(lightingTarget->getHandle());
        driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
        // The scene's buffers outlive this body otherwise, and VMA reports them as unfreed when
        // the allocator goes away -- after which the resource pool's own destructor runs their
        // destructors against a destroyed allocator. Metal tolerated the leak; Vulkan segfaults.
        driver->waitIdle();
        gbuffer.destroy();
        scene.destroy(driver);
        driver->getDeferredReclaimer()->flushBlocking();
    }

    /*
    * @brief Runs the tone-mapping pass over a known HDR colour and checks the mapped result.
    *
    * The interesting property is the white point: the Uncharted 2 curve must be normalized so that
    * an input of 11.2 maps to exactly 1.0. The GLSL this replaced skipped that normalization (and
    * the gamma encode), so whites came out grey -- which is invisible unless something asserts on
    * it, hence this test.
    */
    inline void runTonemapTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR, TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);
        vkm::VkmPipelineStateBase* pso =
            manager.getPipelineState("tonemap_pso", vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        constexpr uint32_t kSize = 16;

        // A uniform HDR source, so every pixel maps identically and one readback texel speaks for
        // the whole image.
        const auto makeSourceTexture = [&](float value) {
            vkm::VkmTextureInfo info{};
            info._flags = static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst));
            info._extent = glm::uvec3(kSize, kSize, 1);
            info._numMipLevels = 1;
            info._numArrayLayers = 1;
            // 8-bit UNORM cannot carry an HDR value above 1, so the source is 32-bit float.
            info._format = vkm::VkmFormat::R32G32B32A32_SFLOAT;
            info._debugName = "TonemapSource";
            vkm::VkmTexture* texture = driver->newTexture(info);
            REQUIRE(texture != nullptr);

            std::vector<float> pixels(static_cast<size_t>(kSize) * kSize * 4);
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                pixels[i + 0] = value;
                pixels[i + 1] = value;
                pixels[i + 2] = value;
                pixels[i + 3] = 1.0f;
            }
            REQUIRE(driver->uploadToTexture(texture->getHandle(), pixels.data(), pixels.size() * sizeof(float)));
            return texture;
        };

        vkm::VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "TonemapSampler";
        vkm::VkmSampler* sampler = driver->newSampler(samplerInfo);
        REQUIRE(sampler != nullptr);

        struct TonemapConstants { float exposureGamma[4]; };
        vkm::VkmBufferInfo constantsInfo{};
        constantsInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
        constantsInfo._size = sizeof(TonemapConstants);
        constantsInfo._debugName = "TonemapConstants";
        vkm::VkmBuffer* constantsBuffer = driver->newBuffer(constantsInfo);
        REQUIRE(constantsBuffer != nullptr);
        // Exposure 1 and gamma 1: an identity gamma isolates the curve itself, so the white-point
        // assertion below is about normalization rather than about the encode.
        const TonemapConstants constants{{1.0f, 1.0f, 0.0f, 0.0f}};
        REQUIRE(driver->uploadToBuffer(constantsBuffer->getHandle(), &constants, sizeof(constants)));

        vkm::VkmTextureInfo targetInfo{};
        targetInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc;
        targetInfo._extent = glm::uvec3(kSize, kSize, 1);
        targetInfo._numMipLevels = 1;
        targetInfo._numArrayLayers = 1;
        targetInfo._format = driver->getSwapChainColorFormat(); // what "swapchain" in the PSO resolves to
        targetInfo._debugName = "TonemapTarget";
        vkm::VkmTexture* target = driver->newTexture(targetInfo);
        REQUIRE(target != nullptr);

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = kSize;
        fbDesc._height = kSize;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._colorAttachments[0] = target->getHandle();

        const auto tonemapValue = [&](float hdrValue) {
            vkm::VkmTexture* source = makeSourceTexture(hdrValue);
            const std::vector<vkm::VkmTableResourceEntry> entries{
                { 0, source->getHandle() },
                { 1, sampler->getHandle() },
                { 2, constantsBuffer->getHandle() },
            };
            std::string tableError;
            vkm::VkmResourceTableBase* table = driver->newResourceTable(pso, vkm::VkmResourceSetKind::PerPass, entries, &tableError);
            REQUIRE_MESSAGE(table != nullptr, tableError);

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
            subGraph->setRenderCallback([pso, table](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pso);
                commandBuffer->bindResourceTable(table);
                commandBuffer->draw(3, 1, 0, 0);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            vkm::VkmTextureReadbackResult readback = driver->readbackTexture(target->getHandle());
            table->destroy();
            delete table;
            driver->getRenderResourcePool()->releaseResource(source->getHandle());
            // The swapchain format may be BGRA, so read the channel the target actually stores
            // red in; a uniform grey source makes the three colour channels equal anyway.
            return readback.pixels[(static_cast<size_t>(kSize / 2) * readback.width + kSize / 2) * readback.channels];
        };

        // The curve's white point. Normalized correctly, 11.2 maps to 1.0 -- i.e. saturated white.
        // Without the normalization the shader this replaces produced roughly 0.8 here, a visibly
        // grey "white" that nothing would have caught.
        CHECK(tonemapValue(11.2f) >= 254);

        // Black maps to black, and a mid value lands strictly between: the curve is monotonic and
        // actually being applied rather than the input passing through.
        CHECK(tonemapValue(0.0f) == 0);
        const uint8_t mid = tonemapValue(1.0f);
        CHECK(mid > 0);
        CHECK(mid < 254);

        driver->getRenderResourcePool()->releaseResource(target->getHandle());
        driver->getRenderResourcePool()->releaseResource(constantsBuffer->getHandle());
        driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
    }
}

#endif // TEST_GBUFFER_RENDER_SHARED_HPP