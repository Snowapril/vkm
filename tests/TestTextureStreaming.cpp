// Copyright (c) 2026 Snowapril

#include <doctest/doctest.h>

#include <vkm/renderer/scene/texture_streamer.h>

#include <cstdint>

/*
* Which mip level a texture should keep resident is ordinary arithmetic over a camera and a
* bounding sphere -- no device, no scene, no textures -- so it is tested here rather than by flying
* a camera around a sample and judging the result by eye.
*
* What makes it worth pinning is that the failure mode is quiet. A selection biased by a constant
* factor still streams, still looks plausible in motion, and only shows up as textures that are
* persistently one level blurrier -- or one level more expensive -- than they should be.
*/

namespace
{
    // 1080p at the engine camera's default 50-degree vertical field of view.
    vkm::VkmTextureStreamingView makeView()
    {
        vkm::VkmTextureStreamingView view;
        view._cameraPosition = glm::vec3(0.0f);
        view._viewportHeight = 1080;
        view._fovYRadians = 0.8726646f;
        return view;
    }

    constexpr uint32_t kTextureWidth = 2048;
    // A 2048-wide chain runs 2048, 1024, ... 1: floor(log2(2048)) + 1.
    constexpr uint32_t kMipCount = 12;
} // namespace

TEST_CASE("vkmSelectStreamingBaseMip keeps level 0 for a surface filling the screen")
{
    const vkm::VkmTextureStreamingView view = makeView();

    // A sphere whose projected diameter comfortably exceeds the texture's width: there is nothing
    // to gain by dropping a level, so the full chain stays.
    const uint32_t level = vkm::vkmSelectStreamingBaseMip(view, /*distance=*/1.0f, /*worldRadius=*/4.0f,
                                                          kTextureWidth, kMipCount, /*mipBias=*/0);
    CHECK(level == 0);
}

TEST_CASE("vkmSelectStreamingBaseMip drops to the coarsest level for a distant surface")
{
    const vkm::VkmTextureStreamingView view = makeView();

    const uint32_t level = vkm::vkmSelectStreamingBaseMip(view, /*distance=*/100000.0f, /*worldRadius=*/0.5f,
                                                          kTextureWidth, kMipCount, /*mipBias=*/0);
    CHECK(level == kMipCount - 1);
}

TEST_CASE("vkmSelectStreamingBaseMip costs one level per doubling of distance")
{
    const vkm::VkmTextureStreamingView view = makeView();
    constexpr float kRadius = 1.0f;

    // Texel density halves with each doubling, which is exactly one mip level -- so whatever the
    // absolute level works out to, the step between two distances an octave apart is 1.
    const uint32_t nearLevel = vkm::vkmSelectStreamingBaseMip(view, 50.0f, kRadius, kTextureWidth, kMipCount, 0);
    const uint32_t midLevel = vkm::vkmSelectStreamingBaseMip(view, 100.0f, kRadius, kTextureWidth, kMipCount, 0);
    const uint32_t farLevel = vkm::vkmSelectStreamingBaseMip(view, 200.0f, kRadius, kTextureWidth, kMipCount, 0);

    // Otherwise the clamp, not the formula, is what is being measured.
    REQUIRE(nearLevel > 0);
    REQUIRE(farLevel < kMipCount - 1);
    CHECK(midLevel - nearLevel == 1);
    CHECK(farLevel - midLevel == 1);
}

TEST_CASE("vkmSelectStreamingBaseMip accounts for the texture's own resolution")
{
    const vkm::VkmTextureStreamingView view = makeView();

    // Same object, same distance, one texture twice as wide as the other. The wider one has one
    // more level to give away before it stops matching the screen.
    const uint32_t small = vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, 1024, kMipCount, 0);
    const uint32_t large = vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, 2048, kMipCount, 0);

    REQUIRE(small > 0);
    CHECK(large - small == 1);
}

TEST_CASE("vkmSelectStreamingBaseMip shifts by the bias and clamps to the chain")
{
    const vkm::VkmTextureStreamingView view = makeView();

    const uint32_t unbiased = vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, kTextureWidth, kMipCount, 0);
    REQUIRE(unbiased > 1);
    REQUIRE(unbiased < kMipCount - 2);

    CHECK(vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, kTextureWidth, kMipCount, -1) == unbiased - 1);
    CHECK(vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, kTextureWidth, kMipCount, 1) == unbiased + 1);

    // A bias large enough to run off either end lands on the end rather than wrapping.
    CHECK(vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, kTextureWidth, kMipCount, -64) == 0);
    CHECK(vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, kTextureWidth, kMipCount, 64) == kMipCount - 1);
}

TEST_CASE("vkmSelectStreamingBaseMip keeps everything when there is nothing to measure against")
{
    vkm::VkmTextureStreamingView view = makeView();

    SUBCASE("a viewport with no height")
    {
        view._viewportHeight = 0;
        CHECK(vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, kTextureWidth, kMipCount, 0) == 0);
    }

    SUBCASE("a camera with no field of view")
    {
        view._fovYRadians = 0.0f;
        CHECK(vkm::vkmSelectStreamingBaseMip(view, 100.0f, 1.0f, kTextureWidth, kMipCount, 0) == 0);
    }

    SUBCASE("an object with no extent")
    {
        CHECK(vkm::vkmSelectStreamingBaseMip(view, 100.0f, 0.0f, kTextureWidth, kMipCount, 0) == 0);
    }

    SUBCASE("a single-level chain has nothing to drop to")
    {
        CHECK(vkm::vkmSelectStreamingBaseMip(view, 100000.0f, 0.5f, kTextureWidth, 1, 0) == 0);
    }
}

TEST_CASE("vkmSelectStreamingBaseMip survives a camera inside the bounding sphere")
{
    const vkm::VkmTextureStreamingView view = makeView();

    // The caller subtracts the radius from the centre distance, so a camera inside the bounds
    // hands this a negative distance. It must clamp rather than divide by zero or go negative.
    const uint32_t level = vkm::vkmSelectStreamingBaseMip(view, /*distance=*/-5.0f, /*worldRadius=*/10.0f,
                                                          kTextureWidth, kMipCount, 0);
    CHECK(level == 0);
}
