#ifndef TEST_PROBE_VOLUME_SHARED_HPP
#define TEST_PROBE_VOLUME_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/probe_volume.h>

#include <set>

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
}

#endif // TEST_PROBE_VOLUME_SHARED_HPP
