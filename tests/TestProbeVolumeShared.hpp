#ifndef TEST_PROBE_VOLUME_SHARED_HPP
#define TEST_PROBE_VOLUME_SHARED_HPP

#include <doctest/doctest.h>

#include "TestHalfFloatShared.hpp"

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/vertex_layout.h>
#include <vkm/renderer/probe_volume.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <vector>

#include <set>

namespace vkmtest
{
    // Mirrors vkm_gbuffer.hlsli's encoding, so the authored normals below are what the shader's
    // decoder expects.
    inline glm::vec2 encodeOctahedral(const glm::vec3& normal)
    {
        const glm::vec3 n = normal / (std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z));
        glm::vec2 encoded(n.x, n.y);
        if (n.z < 0.0f)
        {
            encoded = glm::vec2((1.0f - std::fabs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f),
                                (1.0f - std::fabs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
        }
        return encoded;
    }
}

/*
* Covers the probe volume's addressing, which is where this goes wrong silently.
*
* An atlas extent that disagrees with the per-probe texel origins, or two probes mapping onto the
* same cell, does not crash or fail validation -- it produces probes that overwrite each other and
* an image that merely looks a bit wrong. So the checks below are about the mapping being a
* bijection into bounds, not about "some texture was created".
*/

namespace vkmtest
{
    inline void runProbeVolumeTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmProbeVolume::Descriptor descriptor;
        // Deliberately not a cube and not powers of two on every axis, so an index-math mistake
        // that happens to work for a symmetric grid still shows up here.
        descriptor._probeCounts = glm::uvec3(4u, 3u, 2u);
        descriptor._spacing = glm::vec3(2.0f, 0.5f, 3.0f);
        descriptor._origin = glm::vec3(-1.0f, 10.0f, 100.0f);
        descriptor._irradianceResolution = 8u;
        descriptor._distanceResolution = 16u;

        vkm::VkmProbeVolume volume;
        REQUIRE(volume.initialize(driver, descriptor));
        CHECK(volume.isValid());
        CHECK(volume.getProbeCount() == 4u * 3u * 2u);

        const uint32_t irradianceCell = descriptor._irradianceResolution + vkm::VkmProbeVolume::kBorderTexels * 2u;
        const uint32_t distanceCell = descriptor._distanceResolution + vkm::VkmProbeVolume::kBorderTexels * 2u;

        SUBCASE("atlas extents account for the border on every probe")
        {
            // 4*3 = 12 cells across, 2 down; a border-less extent would be 96x16 rather than 120x20,
            // and every probe after the first would sample its neighbour.
            CHECK(volume.getIrradianceAtlasExtent() == glm::uvec2(12u * irradianceCell, 2u * irradianceCell));
            CHECK(volume.getDistanceAtlasExtent() == glm::uvec2(12u * distanceCell, 2u * distanceCell));
            CHECK(irradianceCell == 10u);
            CHECK(distanceCell == 18u);
        }

        SUBCASE("probe index and grid coordinate round-trip, x varying fastest")
        {
            CHECK(volume.getProbeCoord(0) == glm::uvec3(0u, 0u, 0u));
            CHECK(volume.getProbeCoord(1) == glm::uvec3(1u, 0u, 0u));
            CHECK(volume.getProbeCoord(4) == glm::uvec3(0u, 1u, 0u));   // wraps x
            CHECK(volume.getProbeCoord(12) == glm::uvec3(0u, 0u, 1u));  // wraps xy
            CHECK(volume.getProbeCoord(23) == glm::uvec3(3u, 2u, 1u));  // last probe
        }

        SUBCASE("probe positions follow the grid origin and spacing")
        {
            CHECK(volume.getProbePosition(0) == descriptor._origin);
            // Probe (3, 2, 1) at the far corner.
            const glm::vec3 expected = descriptor._origin + glm::vec3(3.0f, 2.0f, 1.0f) * descriptor._spacing;
            CHECK(volume.getProbePosition(23).x == doctest::Approx(expected.x));
            CHECK(volume.getProbePosition(23).y == doctest::Approx(expected.y));
            CHECK(volume.getProbePosition(23).z == doctest::Approx(expected.z));
        }

        SUBCASE("every probe gets a distinct in-bounds cell")
        {
            // The property that matters: the mapping is injective and lands inside the atlas.
            // Two probes sharing a cell is invisible except as subtly wrong lighting.
            const glm::uvec2 extent = volume.getIrradianceAtlasExtent();
            std::set<std::pair<uint32_t, uint32_t>> origins;
            for (uint32_t probe = 0; probe < volume.getProbeCount(); ++probe)
            {
                const glm::uvec2 origin = volume.getIrradianceProbeTexelOrigin(probe);
                CHECK(origin.x + irradianceCell <= extent.x);
                CHECK(origin.y + irradianceCell <= extent.y);
                origins.insert({origin.x, origin.y});
            }
            CHECK(origins.size() == volume.getProbeCount());
        }

        SUBCASE("both atlas textures are distinct and live")
        {
            std::set<uint64_t> ids;
            for (vkm::VkmResourceHandle handle : {volume.getIrradianceTexture(), volume.getDistanceTexture()})
            {
                REQUIRE(handle.isValid());
                CHECK(driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(handle) != nullptr);
                ids.insert(handle.id);
            }
            CHECK(ids.size() == 2);
        }

        SUBCASE("a degenerate descriptor is rejected rather than allocating nothing")
        {
            vkm::VkmProbeVolume empty;
            vkm::VkmProbeVolume::Descriptor bad = descriptor;
            bad._probeCounts.y = 0u;
            CHECK_FALSE(empty.initialize(driver, bad));

            bad = descriptor;
            bad._irradianceResolution = 0u;
            CHECK_FALSE(empty.initialize(driver, bad));
        }

        volume.destroy();
        CHECK_FALSE(volume.isValid());
    }

    /*
    * @brief Fills the atlases from the CPU with known values and checks what the lookup returns.
    *
    * Filling from the CPU rather than from a probe update pass is deliberate: it isolates the read
    * path, so a failure here is the addressing, the octahedral mapping or the Chebyshev test --
    * not whatever the update wrote. The two cases below are the ones that matter, because both
    * fail *plausibly*: a lookup returning the wrong probe still returns light, and a lookup that
    * ignores visibility still looks lit, just through walls.
    */
    inline void runProbeLightingTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kSize = 32;

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR, TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);
        vkm::VkmPipelineStateBase* pso =
            manager.getPipelineState("probe_lighting_pso", vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        // A grid comfortably surrounding the plane the test shades, so every lookup finds eight
        // in-bounds probes and the result is not a boundary artifact.
        vkm::VkmProbeVolume::Descriptor descriptor;
        descriptor._probeCounts = glm::uvec3(4u, 4u, 4u);
        descriptor._spacing = glm::vec3(4.0f);
        descriptor._origin = glm::vec3(-6.0f, -6.0f, -6.0f);
        vkm::VkmProbeVolume volume;
        REQUIRE(volume.initialize(driver, descriptor));

        // Uniform atlases: every probe, every direction, the same value. Any addressing mistake
        // still lands on *some* probe, so a non-uniform fill could pass by accident -- what this
        // isolates instead is whether the lookup runs and how it weights.
        const auto fillAtlas = [&](vkm::VkmResourceHandle handle, const glm::uvec2& extent,
                                   float r, float g, float b, float a) {
            std::vector<float> pixels(static_cast<size_t>(extent.x) * extent.y * 4);
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                pixels[i + 0] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = a;
            }
            // The atlases are RGBA16F; uploadToTexture takes tightly packed source data matching
            // the texture format, so convert to half via a staging float image is not possible --
            // upload the float bits the format expects instead.
            std::vector<uint16_t> halfPixels(pixels.size());
            for (size_t i = 0; i < pixels.size(); ++i)
            {
                halfPixels[i] = encodeHalf(pixels[i]);
            }
            REQUIRE(driver->uploadToTexture(handle, halfPixels.data(), halfPixels.size() * sizeof(uint16_t)));
        };

        vkm::VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "ProbeLightingSampler";
        vkm::VkmSampler* sampler = driver->newSampler(samplerInfo);
        REQUIRE(sampler != nullptr);

        // Surface inputs authored directly rather than rendered: this test is about the probe
        // lookup, not rasterization. Standalone textures rather than a VkmGBuffer, because a
        // G-buffer is rendered into and so carries no TransferDst -- adding one for a test's
        // convenience would be the wrong reason to widen its usage flags.
        const auto makeUploadableTexture = [&](vkm::VkmFormat format, const char* name) {
            vkm::VkmTextureInfo info{};
            info._flags = static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst));
            info._extent = glm::uvec3(kSize, kSize, 1);
            info._numMipLevels = 1;
            info._numArrayLayers = 1;
            info._format = format;
            info._debugName = name;
            vkm::VkmTexture* texture = driver->newTexture(info);
            REQUIRE(texture != nullptr);
            return texture;
        };
        vkm::VkmTexture* normalTexture =
            makeUploadableTexture(vkm::VkmGBuffer::getTargetFormat(vkm::VkmGBuffer::Target::Normal), "ProbeTestNormal");
        // Stands in for the G-buffer's MotionMetallic target, whose .w carries camera distance.
        vkm::VkmTexture* motionTexture = makeUploadableTexture(
            vkm::VkmGBuffer::getTargetFormat(vkm::VkmGBuffer::Target::MotionMetallic), "ProbeTestMotion");

        const glm::mat4 viewProjection = glm::mat4(1.0f); // identity: NDC == world, depth 0.5 -> z 0.5
        vkm::VkmFrameConstants frameConstants{};
        frameConstants._viewProjection = viewProjection;
        frameConstants._inverseViewProjection = glm::inverse(viewProjection);
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        {
            // Normals: +Z everywhere, octahedrally packed the way gbuffer.hlsl writes them.
            const glm::vec2 oct = encodeOctahedral(glm::vec3(0.0f, 0.0f, 1.0f));
            std::vector<uint16_t> normals(static_cast<size_t>(kSize) * kSize * 4);
            for (size_t i = 0; i < normals.size(); i += 4)
            {
                normals[i + 0] = encodeHalf(oct.x);
                normals[i + 1] = encodeHalf(oct.y);
                normals[i + 2] = encodeHalf(oct.x);
                normals[i + 3] = encodeHalf(oct.y);
            }
            REQUIRE(driver->uploadToTexture(normalTexture->getHandle(),
                                            normals.data(), normals.size() * sizeof(uint16_t)));

            // Camera distance 0.5 in .w. With an identity view-projection the camera sits at the
            // origin looking down +Z, so the reconstructed position is about (0, 0, 0.5) -- well
            // inside the probe grid below.
            std::vector<uint16_t> motion(static_cast<size_t>(kSize) * kSize * 4, 0);
            for (size_t i = 3; i < motion.size(); i += 4)
            {
                motion[i] = encodeHalf(0.5f);
            }
            REQUIRE(driver->uploadToTexture(motionTexture->getHandle(), motion.data(),
                                            motion.size() * sizeof(uint16_t)));
        }

        vkm::VkmBufferInfo volumeBufferInfo{};
        volumeBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
        volumeBufferInfo._size = sizeof(vkm::VkmProbeVolumeConstants);
        volumeBufferInfo._debugName = "ProbeVolumeConstants";
        vkm::VkmBuffer* volumeBuffer = driver->newBuffer(volumeBufferInfo);
        REQUIRE(volumeBuffer != nullptr);
        const vkm::VkmProbeVolumeConstants volumeConstants = volume.makeConstants();
        REQUIRE(driver->uploadToBuffer(volumeBuffer->getHandle(), &volumeConstants, sizeof(volumeConstants)));

        vkm::VkmTextureInfo targetInfo{};
        targetInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc;
        targetInfo._extent = glm::uvec3(kSize, kSize, 1);
        targetInfo._numMipLevels = 1;
        targetInfo._numArrayLayers = 1;
        targetInfo._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        targetInfo._debugName = "ProbeLightingTarget";
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

        const auto shade = [&]() {
            const std::vector<vkm::VkmTableResourceEntry> entries{
                { 0, normalTexture->getHandle() },
                { 1, motionTexture->getHandle() },
                { 2, volume.getIrradianceTexture() },
                { 3, volume.getDistanceTexture() },
                { 4, sampler->getHandle() },
                { 5, volumeBuffer->getHandle() },
                { 6, volume.getProbeOffsetTexture() },
            };
            std::string tableError;
            vkm::VkmResourceTableBase* table = driver->newResourceTable(pso, vkm::VkmResourceSetKind::PerPass, entries, &tableError);
            REQUIRE_MESSAGE(table != nullptr, tableError);

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
            for (vkm::VkmResourceHandle sampled : { normalTexture->getHandle(), motionTexture->getHandle(),
                                                    volume.getIrradianceTexture(), volume.getDistanceTexture(),
                                                    volume.getProbeOffsetTexture() })
            {
                subGraph->addReferencedResource(sampled, vkm::VkmResourceAccess::ShaderSampledRead);
            }
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
            return readback;
        };

        const auto centerTexel = [&](const vkm::VkmTextureReadbackResult& readback) {
            return &readback.pixels[(static_cast<size_t>(kSize / 2) * readback.width + kSize / 2) * readback.channels];
        };

        SUBCASE("a visible probe grid delivers its irradiance")
        {
            fillAtlas(volume.getIrradianceTexture(), volume.getIrradianceAtlasExtent(), 0.25f, 0.5f, 0.75f, 1.0f);
            // Mean distance far beyond any probe, so Chebyshev never rejects: nothing is occluded.
            fillAtlas(volume.getDistanceTexture(), volume.getDistanceAtlasExtent(), 1000.0f, 1000000.0f, 0.0f, 1.0f);

            const vkm::VkmTextureReadbackResult readback = shade();
            REQUIRE(readback.channels == 8);
            const uint8_t* texel = centerTexel(readback);

            // The weights are normalized, so a uniform grid must return the probe value itself
            // rather than some multiple of it -- which is what catches a missing normalization.
            CHECK(readHalfComponent(texel, 0) == doctest::Approx(0.25f).epsilon(0.02));
            CHECK(readHalfComponent(texel, 1) == doctest::Approx(0.5f).epsilon(0.02));
            CHECK(readHalfComponent(texel, 2) == doctest::Approx(0.75f).epsilon(0.02));
        }

        SUBCASE("probes the surface cannot see contribute nothing")
        {
            fillAtlas(volume.getIrradianceTexture(), volume.getIrradianceAtlasExtent(), 0.25f, 0.5f, 0.75f, 1.0f);
            // Every probe reports having seen something almost immediately in front of it, with
            // near-zero variance -- i.e. a wall between it and anything further away. The surface
            // is metres from its probes, so all eight must be rejected.
            fillAtlas(volume.getDistanceTexture(), volume.getDistanceAtlasExtent(), 0.01f, 0.0001f, 0.0f, 1.0f);

            const vkm::VkmTextureReadbackResult readback = shade();
            const uint8_t* texel = centerTexel(readback);

            // This is the leak the distance atlas exists to prevent. Without the Chebyshev term the
            // trilinear weights alone would return the full 0.25/0.5/0.75 here.
            CHECK(readHalfComponent(texel, 0) < 0.02f);
            CHECK(readHalfComponent(texel, 1) < 0.02f);
            CHECK(readHalfComponent(texel, 2) < 0.02f);
        }

        SUBCASE("an authored probe offset moves what the visibility test measures against")
        {
            fillAtlas(volume.getIrradianceTexture(), volume.getIrradianceAtlasExtent(), 0.25f, 0.5f, 0.75f, 1.0f);
            // Every probe reports a surface 1 unit away with no variance, so it accepts a query
            // closer than that and rejects anything further. The eight probes around the shaded
            // point sit 2.9 to 4.5 units from it on the grid, so on the grid every one is rejected.
            fillAtlas(volume.getDistanceTexture(), volume.getDistanceAtlasExtent(), 1.0f, 1.0f, 0.0f, 1.0f);

            const vkm::VkmTextureReadbackResult onGrid = shade();
            CHECK(readHalfComponent(centerTexel(onGrid), 0) < 0.02f);

            // Same probes, same atlases, moved to half a unit from the query point. The only thing
            // that can turn the rejection into an acceptance is the lookup measuring against the
            // moved position; a lookup that offsets only the capture would still read black here.
            // The bias is 0.25 * min(spacing) = 1.0, so the query sits at (0, 0, 1.5).
            const glm::vec3 nearQuery(0.0f, 0.0f, 2.0f);
            for (uint32_t probeIndex = 0; probeIndex < volume.getProbeCount(); ++probeIndex)
            {
                volume.setProbeOffset(probeIndex, nearQuery - volume.getProbeGridPosition(probeIndex));
            }
            REQUIRE(volume.uploadProbeOffsets());
            CHECK(volume.hasProbeOffsets());

            const vkm::VkmTextureReadbackResult offGrid = shade();
            const uint8_t* texel = centerTexel(offGrid);
            CHECK(readHalfComponent(texel, 0) == doctest::Approx(0.25f).epsilon(0.02));
            CHECK(readHalfComponent(texel, 1) == doctest::Approx(0.5f).epsilon(0.02));
            CHECK(readHalfComponent(texel, 2) == doctest::Approx(0.75f).epsilon(0.02));

            volume.clearProbeOffsets();
            REQUIRE(volume.uploadProbeOffsets());
        }

        driver->getRenderResourcePool()->releaseResource(target->getHandle());
        driver->getRenderResourcePool()->releaseResource(volumeBuffer->getHandle());
        driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
        driver->getRenderResourcePool()->releaseResource(motionTexture->getHandle());
        driver->getRenderResourcePool()->releaseResource(normalTexture->getHandle());
        volume.destroy();
    }

    /*
    * @brief Captures a scene from one probe into a packed six-face target and checks the faces.
    *
    * The whole point of this pass is that all six faces share ONE render pass, aimed by
    * setViewportAndScissor. So the assertions are per face: the face pointing at geometry records
    * it, the faces pointing away stay empty, and the recorded distance matches where the geometry
    * actually is. A capture that rendered every face with the same matrix -- the likely failure if
    * the pushed face index does not reach the vertex shader -- would light all six identically and
    * otherwise look fine.
    */
    inline void runProbeCaptureTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kFaceSize = 16;
        constexpr uint32_t kFacesX = 3;
        constexpr uint32_t kFacesY = 2;

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
        const std::string psoName =
            std::string("probe_capture_pso[") +
            vkm::vkmVertexLayoutPresetName(vkm::VkmVertexLayoutPreset::StandardPBR) + "]";
        vkm::VkmPipelineStateBase* pso = manager.getPipelineState(psoName, vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        REQUIRE(scene.build(driver, &manager, &error));

        // The fixture triangle spans x,y in [0,1] at z = 0. Put the probe a little in front of it
        // on -Z, so exactly one face (+Z) looks at it and the opposite face (-Z) sees nothing.
        const glm::vec3 probePosition(0.5f, 0.5f, -2.0f);
        const float expectedDistance = 2.0f;
        scene.setObjectTransform(0, glm::mat4(1.0f));

        // Built at the origin, because the capture shader works in probe-relative space and the
        // probe's position arrives through push constants (see VkmProbeCaptureConstants).
        vkm::VkmProbeCaptureConstants captureConstants{};
        vkm::vkmBuildProbeFaceViewProjections(glm::vec3(0.0f), 0.05f, 100.0f, captureConstants._faceViewProjection);

        vkm::VkmBufferInfo captureBufferInfo{};
        captureBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
        captureBufferInfo._size = sizeof(vkm::VkmProbeCaptureConstants);
        captureBufferInfo._debugName = "ProbeCaptureConstants";
        vkm::VkmBuffer* captureBuffer = driver->newBuffer(captureBufferInfo);
        REQUIRE(captureBuffer != nullptr);
        REQUIRE(driver->uploadToBuffer(captureBuffer->getHandle(), &captureConstants, sizeof(captureConstants)));

        std::string tableError;
        vkm::VkmResourceTableBase* table = driver->newResourceTable(
            pso, vkm::VkmResourceSetKind::PerPass, {{ 0, captureBuffer->getHandle() }}, &tableError);
        REQUIRE_MESSAGE(table != nullptr, tableError);

        const glm::uvec2 captureExtent(kFaceSize * kFacesX, kFaceSize * kFacesY);
        const auto makeTarget = [&](vkm::VkmFormat format, vkm::VkmResourceCreateInfo extraFlags, const char* name) {
            vkm::VkmTextureInfo info{};
            info._flags = extraFlags;
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
            "ProbeCaptureColor");
        vkm::VkmTexture* captureDepth = makeTarget(
            vkm::VkmFormat::D32_SFLOAT, vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment, "ProbeCaptureDepth");

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

        vkm::VkmFrameData frameData;
        // Frustum planes must be the probe's, not the camera's: the cull pass runs against them,
        // and culling with a stale frustum would reject the very geometry the probe should see.
        // The matrices are origin-relative now, so the frustum has to be moved onto the probe.
        vkm::vkmExtractFrustumPlanes(
            captureConstants._faceViewProjection[4] * glm::translate(glm::mat4(1.0f), -probePosition),
            frameData._frustumPlanes);
        // lightDirection points TOWARDS the light (the engine's convention), and the fixture's
        // normals are +Z, so this is what saturates nDotL and makes the recorded radiance the
        // material colour rather than zero.
        frameData._lightDirection = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);

        std::vector<vkm::VkmResourceAccessDeclaration> referenced;

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

        auto* captureSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        referenced.clear();
        scene.collectReferencedResources(vkm::VkmScene::ReferencePhase::Draw, &referenced);
        captureSubGraph->addReferencedResources(referenced);
        captureSubGraph->setRenderCallback([&scene, pso, table, probePosition](vkm::VkmCommandBufferBase* commandBuffer) {
            // All six faces in one pass: aim the viewport, push which probe and face, draw.
            for (uint32_t face = 0; face < 6; ++face)
            {
                const uint32_t tileX = (face % kFacesX) * kFaceSize;
                const uint32_t tileY = (face / kFacesX) * kFaceSize;
                commandBuffer->setViewportAndScissor(static_cast<int32_t>(tileX), static_cast<int32_t>(tileY),
                                                     kFaceSize, kFaceSize);

                vkm::VkmProbeCapturePushConstants push{};
                push._probePositionWorld = probePosition;
                push._faceIndex = face;

                scene.recordDrawBatches(
                    commandBuffer,
                    [pso](const vkm::VkmScene::DrawBatch&) { return pso; },
                    [table, &push](vkm::VkmCommandBufferBase* cb, const vkm::VkmScene::DrawBatch&) {
                        cb->bindResourceTable(table);
                        cb->setPushConstants(&push, sizeof(push));
                    });
            }
        });

        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(captureTarget->getHandle());
        REQUIRE(readback.channels == 8); // bytes per texel, RGBA16F

        const auto faceCenterTexel = [&](uint32_t face) {
            const uint32_t x = (face % kFacesX) * kFaceSize + kFaceSize / 2;
            const uint32_t y = (face / kFacesX) * kFaceSize + kFaceSize / 2;
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };

        // Face 4 is +Z, the one looking at the triangle.
        const uint8_t* facingTexel = faceCenterTexel(4);
        CHECK(readHalfComponent(facingTexel, 0) > 0.0f);
        CHECK(readHalfComponent(facingTexel, 3) == doctest::Approx(expectedDistance).epsilon(0.05));

        // Face 5 is -Z, pointing directly away. Empty here is what shows each face used its own
        // matrix: one shared matrix would have drawn the triangle into all six tiles.
        const uint8_t* awayTexel = faceCenterTexel(5);
        CHECK(readHalfComponent(awayTexel, 0) == 0.0f);
        CHECK(readHalfComponent(awayTexel, 3) == 0.0f);

        table->destroy();
        delete table;
        // VkmScene's destructor is defaulted, so without this its buffers outlive the driver and
        // Vulkan's allocator aborts at teardown. Latent while this test was Metal-only.
        scene.destroy(driver);
        driver->getRenderResourcePool()->releaseResource(captureDepth->getHandle());
        driver->getRenderResourcePool()->releaseResource(captureTarget->getHandle());
        driver->getRenderResourcePool()->releaseResource(captureBuffer->getHandle());
    }

    /*
    * @brief Blends a synthetic capture into the irradiance atlas and checks what landed.
    *
    * The capture is authored rather than rendered, so a failure here is the integration, the
    * octahedral mapping or the border handling -- not the scene draw, which the capture test
    * already covers. It is filled so that one hemisphere is bright and the other is black, which
    * is what makes the result directional: an integration that ignored the cosine term, or an
    * octahedral mapping that folded the wrong way, would produce a uniform atlas that still looks
    * like "the probe has light in it".
    */
    inline void runProbeBlendTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kFaceSize = 16;
        constexpr uint32_t kFacesPerRow = 3;
        constexpr uint32_t kResolution = 8;
        constexpr uint32_t kCellSize = kResolution + 2; // one border texel each side

        vkm::VkmPipelineStateManager manager(driver);
        std::string psoError;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR, TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &psoError),
                        psoError);
        vkm::VkmPipelineStateBase* pso =
            manager.getPipelineState("probe_blend_pso[irradiance]", vkm::VkmPipelineStateOrigin::Engine);
        REQUIRE(pso != nullptr);

        const glm::uvec2 captureExtent(kFaceSize * kFacesPerRow, kFaceSize * 2);

        vkm::VkmProbeBlendConstants blendConstants{};
        vkm::vkmBuildProbeFaceViewProjections(glm::vec3(0.0f), 0.05f, 100.0f, blendConstants._faceViewProjection);
        blendConstants._blendParams = glm::vec4(static_cast<float>(kResolution), 0.0f,
                                                static_cast<float>(kFacesPerRow), static_cast<float>(kFaceSize));
        blendConstants._captureAtlasSize = glm::vec4(static_cast<float>(captureExtent.x),
                                                     static_cast<float>(captureExtent.y), 0.0f, 0.0f);

        // Capture: face 4 (+Z) fully bright, every other face black. So the probe "saw" light in
        // exactly one hemisphere.
        std::vector<uint16_t> capturePixels(static_cast<size_t>(captureExtent.x) * captureExtent.y * 4, 0);
        for (uint32_t y = 0; y < kFaceSize; ++y)
        {
            for (uint32_t x = 0; x < kFaceSize; ++x)
            {
                // Face 4 sits at tile (1, 1) in a 3-per-row layout.
                const uint32_t px = 1 * kFaceSize + x;
                const uint32_t py = 1 * kFaceSize + y;
                const size_t texel = (static_cast<size_t>(py) * captureExtent.x + px) * 4;
                capturePixels[texel + 0] = encodeHalf(1.0f);
                capturePixels[texel + 1] = encodeHalf(1.0f);
                capturePixels[texel + 2] = encodeHalf(1.0f);
                capturePixels[texel + 3] = encodeHalf(5.0f); // distance, unused by this permutation
            }
        }

        const auto makeTexture = [&](const glm::uvec2& extent, vkm::VkmResourceCreateInfo flags, const char* name) {
            vkm::VkmTextureInfo info{};
            info._flags = flags;
            info._extent = glm::uvec3(extent, 1);
            info._numMipLevels = 1;
            info._numArrayLayers = 1;
            info._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
            info._debugName = name;
            vkm::VkmTexture* texture = driver->newTexture(info);
            REQUIRE(texture != nullptr);
            return texture;
        };

        vkm::VkmTexture* captureTexture = makeTexture(
            captureExtent,
            static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst)),
            "ProbeBlendCapture");
        REQUIRE(driver->uploadToTexture(captureTexture->getHandle(), capturePixels.data(),
                                        capturePixels.size() * sizeof(uint16_t)));

        // A single-probe atlas: the viewport covers exactly one cell, which is what the shader
        // assumes its UV spans.
        vkm::VkmTexture* atlasTexture = makeTexture(
            glm::uvec2(kCellSize, kCellSize),
            static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowColorAttachment) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferSrc)),
            "ProbeBlendAtlas");

        vkm::VkmBufferInfo blendBufferInfo{};
        blendBufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
        blendBufferInfo._size = sizeof(vkm::VkmProbeBlendConstants);
        blendBufferInfo._debugName = "ProbeBlendConstants";
        vkm::VkmBuffer* blendBuffer = driver->newBuffer(blendBufferInfo);
        REQUIRE(blendBuffer != nullptr);
        REQUIRE(driver->uploadToBuffer(blendBuffer->getHandle(), &blendConstants, sizeof(blendConstants)));

        vkm::VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "ProbeBlendSampler";
        vkm::VkmSampler* sampler = driver->newSampler(samplerInfo);
        REQUIRE(sampler != nullptr);

        std::string tableError;
        vkm::VkmResourceTableBase* table = driver->newResourceTable(
            pso, vkm::VkmResourceSetKind::PerPass,
            {{ 0, captureTexture->getHandle() },
             { 1, sampler->getHandle() },
             { 2, blendBuffer->getHandle() }},
            &tableError);
        REQUIRE_MESSAGE(table != nullptr, tableError);

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = kCellSize;
        fbDesc._height = kCellSize;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._colorAttachments[0] = atlasTexture->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* blendSubGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        blendSubGraph->addReferencedResource(captureTexture->getHandle(),
                                             vkm::VkmResourceAccess::ShaderSampledRead);
        blendSubGraph->setRenderCallback([pso, table](vkm::VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            commandBuffer->bindResourceTable(table);

            // Hysteresis 0 so this measures the integration alone: the blend state then weights the
            // new value 1 and the cleared attachment 0, making the pass a plain overwrite.
            vkm::VkmProbeBlendPushConstants push{};
            push._captureTileBase = 0;
            push._hysteresis = 0.0f;
            commandBuffer->setPushConstants(&push, sizeof(push));

            commandBuffer->draw(3, 1, 0, 0);
        });
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(atlasTexture->getHandle());
        REQUIRE(readback.channels == 8);
        const auto texelAt = [&](uint32_t x, uint32_t y) {
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };

        // The octahedral centre is +Z, the hemisphere the capture lit, so the middle of the cell
        // must carry most of the energy.
        const float centre = readHalfComponent(texelAt(kCellSize / 2, kCellSize / 2), 0);
        // The cell's interior corners are -Z, the hemisphere that was black.
        const float corner = readHalfComponent(texelAt(1, 1), 0);

        CHECK(centre > 0.2f);
        // Directional, not uniform. An integration ignoring the cosine term, or an octahedral fold
        // going the wrong way, would make these two equal while still looking "lit".
        CHECK(corner < centre * 0.5f);

        // The border replicates the opposite edge, so a border texel must not be black while its
        // mirrored interior neighbour is lit -- the seam artifact the border exists to prevent.
        const float topBorder = readHalfComponent(texelAt(kCellSize / 2, 0), 0);
        const float mirroredInterior = readHalfComponent(texelAt(kCellSize - 1 - kCellSize / 2, 1), 0);
        CHECK(topBorder == doctest::Approx(mirroredInterior).epsilon(0.05));

        table->destroy();
        delete table;
        for (vkm::VkmResourceHandle handle : {atlasTexture->getHandle(), captureTexture->getHandle(),
                                              blendBuffer->getHandle(), sampler->getHandle()})
        {
            driver->getRenderResourcePool()->releaseResource(handle);
        }
    }
}

#endif // TEST_PROBE_VOLUME_SHARED_HPP