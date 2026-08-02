#ifndef TEST_GBUFFER_RENDER_SHARED_HPP
#define TEST_GBUFFER_RENDER_SHARED_HPP

#include <doctest/doctest.h>

#include "TestHalfFloatShared.hpp"

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <cmath>
#include <string>
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
    inline glm::mat4 fillGBuffer(vkm::VkmDriverBase* driver,
                                 vkm::VkmPipelineStateManager& manager,
                                 vkm::VkmScene& scene,
                                 vkm::VkmGBuffer& gbuffer)
    {
        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf",
                                     &model, &error, importOptions));

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
        frameConstants._prevViewProjection = viewProjection;
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        std::vector<vkm::VkmResourceHandle> referenced;
        scene.collectReferencedResources(&referenced);

        const vkm::VkmFrameBufferDescriptor fbDesc = gbuffer.makeFrameBufferDescriptor();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* updateSubGraph = renderGraph.beginTransferSubGraph("SceneUpdate");
        for (vkm::VkmResourceHandle handle : referenced)
        {
            updateSubGraph->addReferencedResource(handle);
        }
        updateSubGraph->setTransferCallback([&scene, &frameData](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData);
        });

        auto* cullSubGraph = renderGraph.beginComputeSubGraph("SceneCull");
        for (vkm::VkmResourceHandle handle : referenced)
        {
            cullSubGraph->addReferencedResource(handle);
        }
        cullSubGraph->setComputeCallback([&scene](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordCull(commandBuffer);
        });

        auto* drawSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        for (vkm::VkmResourceHandle handle : referenced)
        {
            drawSubGraph->addReferencedResource(handle);
        }
        drawSubGraph->setRenderCallback([&scene, pso](vkm::VkmCommandBufferBase* commandBuffer) {
            scene.recordDrawBatches(commandBuffer, [pso](const vkm::VkmScene::DrawBatch&) { return pso; });
        });

        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        return viewProjection;
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

        gbuffer.destroy();
    }

    /*
    * @brief Fills the G-buffer, hands it to the fullscreen lighting pass through descriptor set 2,
    * and checks the shaded result.
    *
    * Several threads meet here, which is the point of testing them together:
    *   - set 2 carrying sampled textures, a sampler and a uniform buffer (the buffer-only test
    *     never exercised the texture or sampler paths, and their Metal indices are pinned by
    *     vkm-compiler from the same declaration the runtime binds against),
    *   - barrierTextureForShaderRead handing an attachment over to be sampled, which until now was
    *     only checked by its tracked layout rather than by pixels, and
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
            };
            std::string tableError;
            vkm::VkmResourceTableBase* table =
                driver->newResourceTable(lightingPso, vkm::VkmResourceSetKind::PerPass, entries, &tableError);
            REQUIRE_MESSAGE(table != nullptr, tableError);

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* barrierSubGraph = renderGraph.beginComputeSubGraph("GBufferToShaderRead");
            barrierSubGraph->setComputeCallback([&gbuffer](vkm::VkmCommandBufferBase* commandBuffer) {
                // The G-buffer pass left these as attachments; this is the hand-off to sampling.
                // Colour targets only: the depth attachment is never sampled (see gbuffer.h).
                for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
                {
                    commandBuffer->barrierTextureForShaderRead(
                        gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)));
                }
            });

            auto* lightingSubGraph = renderGraph.beginGraphicsSubGraph(lightingFb);
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

        // Doubling only the set-2 uniform buffer doubles the result. Nothing else changed, so this
        // is what proves that buffer actually reaches the shader rather than the pass running on
        // whatever happened to be bound.
        const vkm::VkmTextureReadbackResult doubled = shadeWith(2.0f);
        CHECK(readHalfComponent(texelAt(doubled, litX, litY), 0) == doctest::Approx(litR * 2.0f).epsilon(0.02));
        CHECK(readHalfComponent(texelAt(doubled, litX, litY), 1) == doctest::Approx(litG * 2.0f).epsilon(0.02));
        CHECK(readHalfComponent(texelAt(doubled, litX, litY), 2) == doctest::Approx(litB * 2.0f).epsilon(0.02));

        driver->getRenderResourcePool()->releaseResource(lightingTarget->getHandle());
        driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
        gbuffer.destroy();
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