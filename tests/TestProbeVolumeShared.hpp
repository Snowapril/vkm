#ifndef TEST_PROBE_VOLUME_SHARED_HPP
#define TEST_PROBE_VOLUME_SHARED_HPP

#include <doctest/doctest.h>

#include "TestHalfFloatShared.hpp"

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/per_pass_resource_table.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/probe_volume.h>

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

        SUBCASE("both atlases are double-buffered and flip together")
        {
            const vkm::VkmResourceHandle irradiance = volume.getIrradianceTexture();
            const vkm::VkmResourceHandle distance = volume.getDistanceTexture();
            REQUIRE(irradiance != volume.getPrevIrradianceTexture());

            volume.advanceFrame();
            // An update reads the previous values and blends into the current ones, so what was
            // written must be what the next update reads.
            CHECK(volume.getPrevIrradianceTexture() == irradiance);
            CHECK(volume.getPrevDistanceTexture() == distance);
            CHECK(volume.getIrradianceTexture() != irradiance);

            volume.advanceFrame();
            CHECK(volume.getIrradianceTexture() == irradiance);
        }

        SUBCASE("all four atlas textures are distinct and live")
        {
            std::set<uint64_t> ids;
            for (vkm::VkmResourceHandle handle : {volume.getIrradianceTexture(), volume.getDistanceTexture(),
                                                  volume.getPrevIrradianceTexture(), volume.getPrevDistanceTexture()})
            {
                REQUIRE(handle.isValid());
                CHECK(driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(handle) != nullptr);
                ids.insert(handle.id);
            }
            CHECK(ids.size() == 4);
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
        // R32F would be the natural depth stand-in, but the engine exposes no single-channel
        // format; RGBA32F carries the value in .r, which is all the shader samples.
        vkm::VkmTexture* depthTexture = makeUploadableTexture(vkm::VkmFormat::R32G32B32A32_SFLOAT, "ProbeTestDepth");

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

            // Depth 0.5, i.e. covered and, through the identity inverse, at world z = 0.5.
            std::vector<float> depths(static_cast<size_t>(kSize) * kSize * 4, 0.5f);
            REQUIRE(driver->uploadToTexture(depthTexture->getHandle(), depths.data(),
                                            depths.size() * sizeof(float)));
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
            const std::vector<vkm::VkmPerPassResourceEntry> entries{
                { 0, normalTexture->getHandle() },
                { 1, depthTexture->getHandle() },
                { 2, volume.getIrradianceTexture() },
                { 3, volume.getDistanceTexture() },
                { 4, sampler->getHandle() },
                { 5, volumeBuffer->getHandle() },
            };
            std::string tableError;
            vkm::VkmPerPassResourceTableBase* table = driver->newPerPassResourceTable(pso, entries, &tableError);
            REQUIRE_MESSAGE(table != nullptr, tableError);

            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* barrierSubGraph = renderGraph.beginComputeSubGraph("ProbeInputsToShaderRead");
            barrierSubGraph->setComputeCallback([&](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->barrierTextureForShaderRead(normalTexture->getHandle());
                commandBuffer->barrierTextureForShaderRead(depthTexture->getHandle());
                commandBuffer->barrierTextureForShaderRead(volume.getIrradianceTexture());
                commandBuffer->barrierTextureForShaderRead(volume.getDistanceTexture());
            });
            auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
            subGraph->setRenderCallback([pso, table](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pso);
                commandBuffer->bindPerPassResources(table);
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

        driver->getRenderResourcePool()->releaseResource(target->getHandle());
        driver->getRenderResourcePool()->releaseResource(volumeBuffer->getHandle());
        driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
        driver->getRenderResourcePool()->releaseResource(depthTexture->getHandle());
        driver->getRenderResourcePool()->releaseResource(normalTexture->getHandle());
        volume.destroy();
    }
}

#endif // TEST_PROBE_VOLUME_SHARED_HPP