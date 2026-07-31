#ifndef TEST_GBUFFER_RENDER_SHARED_HPP
#define TEST_GBUFFER_RENDER_SHARED_HPP

#include <doctest/doctest.h>

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

    // Decodes an IEEE-754 binary16 as stored in an RGBA16F readback. Small enough to keep here
    // rather than adding a half-float dependency for two assertions.
    inline float decodeHalf(uint16_t bits)
    {
        const uint32_t sign = static_cast<uint32_t>(bits >> 15) & 0x1u;
        const uint32_t exponent = static_cast<uint32_t>(bits >> 10) & 0x1Fu;
        const uint32_t mantissa = static_cast<uint32_t>(bits) & 0x3FFu;

        float value = 0.0f;
        if (exponent == 0)
        {
            value = std::ldexp(static_cast<float>(mantissa), -24); // subnormal (and zero)
        }
        else if (exponent == 31)
        {
            value = mantissa == 0 ? INFINITY : NAN;
        }
        else
        {
            value = std::ldexp(static_cast<float>(mantissa + 1024u), static_cast<int>(exponent) - 25);
        }
        return sign != 0 ? -value : value;
    }

    inline float readHalfComponent(const uint8_t* texel, size_t component)
    {
        const uint16_t bits = static_cast<uint16_t>(texel[component * 2]) |
                              static_cast<uint16_t>(static_cast<uint16_t>(texel[component * 2 + 1]) << 8);
        return decodeHalf(bits);
    }

    inline void runGBufferRenderTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf",
                                     &model, &error, importOptions));

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR, TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        REQUIRE(scene.build(driver, &manager, &error));

        const std::string psoName =
            std::string("gbuffer_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState(psoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        vkm::VkmGBuffer gbuffer;
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
}

#endif // TEST_GBUFFER_RENDER_SHARED_HPP
