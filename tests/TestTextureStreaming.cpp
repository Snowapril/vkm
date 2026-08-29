// Copyright (c) 2026 Snowapril

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/texture_streamer.h>

#include <cstdint>
#include <string>

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

/*
* The byte accounting behind the streaming readout. It is the number the whole feature is judged by,
* and it is quietly easy to get wrong: the old computeTextureByteSize counted only level 0, which
* under-reports a full chain by a quarter and would have made the reported saving wrong by the same
* factor in both directions.
*/

TEST_CASE("vkmMipRangeByteSize sums a mip chain rather than its base level")
{
    // 4x4 RGBA8: 16 + 4 + 1 texels over three levels.
    const glm::uvec3 extent(4, 4, 1);
    constexpr uint32_t kBpp = 4;

    CHECK(vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 1) == 16 * kBpp);
    CHECK(vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 3) == (16 + 4 + 1) * kBpp);

    // A range starting mid-chain is the sum of its own levels and nothing above them, which is
    // exactly what a streamed texture holds.
    CHECK(vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 1, 2) == (4 + 1) * kBpp);
    CHECK(vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 2, 1) == 1 * kBpp);

    // Array layers each carry their own copy.
    CHECK(vkm::vkmMipRangeByteSize(extent, 6, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 3) == 6 * (16 + 4 + 1) * kBpp);
}

TEST_CASE("vkmMipRangeByteSize approaches 4/3 of the base level for a full chain")
{
    // The classic result, and the reason counting level 0 alone under-reports by a quarter.
    const glm::uvec3 extent(1024, 1024, 1);
    const uint64_t base = vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 1);
    const uint64_t chain = vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 11);

    CHECK(base == 1024ull * 1024ull * 4ull);
    CHECK(chain > base);
    CHECK(static_cast<double>(chain) / static_cast<double>(base) == doctest::Approx(4.0 / 3.0).epsilon(0.001));
}

TEST_CASE("vkmMipRangeByteSize handles degenerate inputs without running away")
{
    const glm::uvec3 extent(8, 8, 1);

    SUBCASE("an empty range is no bytes")
    {
        CHECK(vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 0) == 0);
    }

    SUBCASE("no array layers is no bytes")
    {
        CHECK(vkm::vkmMipRangeByteSize(extent, 0, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 4) == 0);
    }

    SUBCASE("an unknown format reports nothing rather than guessing")
    {
        CHECK(vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::Undefined, 0, 4) == 0);
    }

    SUBCASE("levels past the 1x1 tail keep counting 1x1 rather than shifting off the end")
    {
        // Shifting by 32 or more is undefined behaviour, so the guard matters: ask for far more
        // levels than the chain has and every extra one must contribute exactly one texel.
        const uint64_t four = vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 4);
        const uint64_t forty = vkm::vkmMipRangeByteSize(extent, 1, vkm::VkmFormat::R8G8B8A8_UNORM, 0, 40);
        CHECK(four == (64 + 16 + 4 + 1) * 4);
        CHECK(forty == four + 36 * 4);
    }
}

TEST_CASE("computeTextureByteSize covers every declared level")
{
    vkm::VkmTextureInfo info{};
    info._extent = glm::uvec3(4, 4, 1);
    info._numArrayLayers = 1;
    info._format = vkm::VkmFormat::R8G8B8A8_UNORM;

    info._numMipLevels = 1;
    CHECK(vkm::computeTextureByteSize(info) == 16 * 4);

    info._numMipLevels = 3;
    CHECK(vkm::computeTextureByteSize(info) == (16 + 4 + 1) * 4);

    // VkmTextureInfo has no default for _numMipLevels and several call sites clamp it, so a zero
    // must still report the base level rather than collapsing a whole category total to nothing.
    info._numMipLevels = 0;
    CHECK(vkm::computeTextureByteSize(info) == 16 * 4);
}

TEST_CASE("vkmMaterialTextureDebugName names a texture after its file and colour space")
{
    // One file sampled in two colour spaces is two textures, so the name has to separate them or
    // the texture browser lists two rows it calls the same thing.
    CHECK(vkm::vkmMaterialTextureDebugName("/scenes/sponza/curtain_diff.png", true) ==
          "SceneMaterialTexture:curtain_diff(srgb)");
    CHECK(vkm::vkmMaterialTextureDebugName("/scenes/sponza/curtain_diff.png", false) ==
          "SceneMaterialTexture:curtain_diff(linear)");

    SUBCASE("a bare filename, a Windows separator, and a name with no extension all resolve")
    {
        CHECK(vkm::vkmMaterialTextureDebugName("brick.jpg", true) == "SceneMaterialTexture:brick(srgb)");
        CHECK(vkm::vkmMaterialTextureDebugName("C:\\assets\\brick.jpg", true) ==
              "SceneMaterialTexture:brick(srgb)");
        CHECK(vkm::vkmMaterialTextureDebugName("assets/brick", true) == "SceneMaterialTexture:brick(srgb)");
    }

    SUBCASE("a dot in a directory is not mistaken for the extension separator")
    {
        CHECK(vkm::vkmMaterialTextureDebugName("/a.b/brick", true) == "SceneMaterialTexture:brick(srgb)");
    }
}

/*
* The GPU feedback reading, decoded. The shader measures against the texture it sampled, which for a
* streamed texture is already reduced -- so converting back to a chain-absolute level is the one
* place an off-by-one silently turns streaming into a loop that chases itself down to nothing.
*/

TEST_CASE("vkmStreamingBaseMipFromFeedback adds the level the sampled texture already started at")
{
    // Nothing streamed out yet, so the reading is already absolute.
    CHECK(vkm::vkmStreamingBaseMipFromFeedback(/*reported=*/0, /*residentBaseMip=*/0, /*totalMipCount=*/12) == 0);
    CHECK(vkm::vkmStreamingBaseMipFromFeedback(3, 0, 12) == 3);

    // Streamed out by three: the shader's "level 0" is the chain's level 3.
    CHECK(vkm::vkmStreamingBaseMipFromFeedback(0, 3, 12) == 3);
    CHECK(vkm::vkmStreamingBaseMipFromFeedback(2, 3, 12) == 5);
}

TEST_CASE("vkmStreamingBaseMipFromFeedback is a fixed point once the texture holds what was asked")
{
    // The property that keeps the loop stable: stream out to the level feedback asked for, and the
    // next reading of the same surface names that same level rather than a coarser one.
    const uint32_t firstAsk = vkm::vkmStreamingBaseMipFromFeedback(4, 0, kMipCount);
    CHECK(firstAsk == 4);

    // Now resident at 4, that surface reports 0 -- the finest level it currently holds.
    CHECK(vkm::vkmStreamingBaseMipFromFeedback(0, firstAsk, kMipCount) == firstAsk);
}

TEST_CASE("vkmStreamingBaseMipFromFeedback clamps to the chain rather than running past it")
{
    CHECK(vkm::vkmStreamingBaseMipFromFeedback(15, 10, 12) == 11);
    CHECK(vkm::vkmStreamingBaseMipFromFeedback(0, 99, 12) == 11);

    SUBCASE("a reading past the encoding's range saturates instead of wrapping")
    {
        CHECK(vkm::vkmStreamingBaseMipFromFeedback(1000, 0, 12) == 11);
    }

    SUBCASE("a single-level chain has nowhere to go")
    {
        CHECK(vkm::vkmStreamingBaseMipFromFeedback(7, 0, 1) == 0);
    }

    SUBCASE("an empty chain does not underflow computing its last level")
    {
        CHECK(vkm::vkmStreamingBaseMipFromFeedback(7, 0, 0) == 0);
    }
}

TEST_CASE("vkmStreamingBaseMipForProjection floors the level rather than rounding it")
{
    // A projection landing between two levels: half a level of texel density is detail the screen
    // can still show, so it is kept rather than rounded away.
    // width 1024, projected 181 -> log2(1024/181) = 2.50; projected 145 -> 2.82.
    CHECK(vkm::vkmStreamingBaseMipForProjection(181.0f, 1024u, kMipCount, 0) == 2);
    CHECK(vkm::vkmStreamingBaseMipForProjection(145.0f, 1024u, kMipCount, 0) == 2);
    // An exact level is unaffected by the change: 1024/128 is exactly three halvings.
    CHECK(vkm::vkmStreamingBaseMipForProjection(128.0f, 1024u, kMipCount, 0) == 3);
}

TEST_CASE("vkmCombineStreamingBaseMip lets the estimate recover a rebuilt texture the reading cannot")
{
    // The ratchet: a texture rebuilt down to level 4 reports 4 as the finest thing it can name,
    // because its own level 0 is chain level 4. Left to the reading alone it would stay there
    // however close the camera came.
    SUBCASE("a rebuilt texture takes the finer of the two")
    {
        CHECK(vkm::vkmCombineStreamingBaseMip(4, 0, /*sparse=*/false) == 0);
        CHECK(vkm::vkmCombineStreamingBaseMip(4, 2, /*sparse=*/false) == 2);
    }

    SUBCASE("the estimate can never make a rebuilt texture coarser than the reading asked for")
    {
        // This is the direction the reading is trusted in: it sees UV density, grazing angles and
        // occlusion, and the sphere does not.
        CHECK(vkm::vkmCombineStreamingBaseMip(2, 6, /*sparse=*/false) == 2);
    }

    SUBCASE("a sparse texture's reading is authoritative in both directions")
    {
        // It keeps its full extent whatever is backed, so it can always ask for level 0 again and
        // the estimate has nothing to add.
        CHECK(vkm::vkmCombineStreamingBaseMip(4, 0, /*sparse=*/true) == 4);
        CHECK(vkm::vkmCombineStreamingBaseMip(2, 6, /*sparse=*/true) == 2);
    }

    SUBCASE("nothing visible drew it, so there is no estimate to combine")
    {
        CHECK(vkm::vkmCombineStreamingBaseMip(3, vkm::INVALID_VALUE32, /*sparse=*/false) == 3);
        CHECK(vkm::vkmCombineStreamingBaseMip(3, vkm::INVALID_VALUE32, /*sparse=*/true) == 3);
    }
}
