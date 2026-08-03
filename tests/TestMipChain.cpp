// Copyright (c) 2025 Snowapril

#include <doctest/doctest.h>

#include <vkm/renderer/scene/image_loader.h>

#include <cstdint>
#include <vector>

/*
* Mip chains for material textures are built on the CPU and uploaded level by level, so the filter
* itself is ordinary code with no device in sight -- which is the whole reason it is tested here
* rather than by rendering something and squinting at it.
*
* The load-bearing case is the sRGB one. Averaging gamma-encoded values directly averages the wrong
* quantity, and the error is invisible on any solid-colour image: it only shows where a level
* actually mixes different texels. That is precisely the shape of bug the material-texture GPU test
* cannot catch, because its fixture is a solid green square.
*/

namespace
{
    // A `width` x `height` image whose texels alternate between two RGBA colours in a checkerboard,
    // so every 2x2 box the filter takes contains two of each.
    vkm::VkmImageData makeCheckerboard(uint32_t width, uint32_t height,
                                       const uint8_t a[4], const uint8_t b[4])
    {
        vkm::VkmImageData image;
        image._width = width;
        image._height = height;
        image._pixels.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t* source = (((x + y) & 1u) == 0u) ? a : b;
                uint8_t* dst = &image._pixels[(static_cast<size_t>(y) * width + x) * 4];
                for (uint32_t c = 0; c < 4; ++c)
                {
                    dst[c] = source[c];
                }
            }
        }
        return image;
    }
} // namespace

TEST_CASE("vkmMipLevelCount counts the base plus every halving")
{
    CHECK(vkm::vkmMipLevelCount(1, 1) == 1);
    CHECK(vkm::vkmMipLevelCount(2, 2) == 2);
    CHECK(vkm::vkmMipLevelCount(256, 256) == 9);
    // Non-square: the longer edge decides, since the shorter one clamps at 1 and keeps going.
    CHECK(vkm::vkmMipLevelCount(256, 1) == 9);
    CHECK(vkm::vkmMipLevelCount(8, 32) == 6);
}

TEST_CASE("vkmBuildMipChain produces every level down to 1x1")
{
    const uint8_t white[4] = { 255, 255, 255, 255 };
    const uint8_t black[4] = { 0, 0, 0, 255 };
    const vkm::VkmImageData base = makeCheckerboard(8, 8, white, black);

    std::vector<vkm::VkmImageData> levels;
    vkm::vkmBuildMipChain(base, /*srgb=*/false, &levels);

    // The base is the caller's and is not copied back, so the chain is one shorter than the count.
    REQUIRE(levels.size() == vkm::vkmMipLevelCount(8, 8) - 1);
    const uint32_t expected[3][2] = { {4, 4}, {2, 2}, {1, 1} };
    for (size_t i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i]._width == expected[i][0]);
        CHECK(levels[i]._height == expected[i][1]);
        // Every level must be fully populated, or the upload reads past what was written.
        CHECK(levels[i]._pixels.size() ==
              static_cast<size_t>(levels[i]._width) * levels[i]._height * 4);
    }
}

TEST_CASE("vkmBuildMipChain averages sRGB in linear space, not in the encoding")
{
    // Half black, half white. In *linear* light that is 0.5, whose sRGB encoding is ~188 -- not
    // 128, which is what averaging the encoded bytes gives. The gap between 188 and 128 is the
    // entire bug: a chain built the wrong way is visibly too dark, and worst at the coarsest level.
    // Alpha runs opaque/transparent alongside the colour, so the two halves of the claim are
    // separable. Alpha 255 on both texels would make the alpha check vacuous -- the curve maps 255
    // to 255, so a filter that wrongly ran alpha through it would still return 255 and pass.
    const uint8_t white[4] = { 255, 255, 255, 255 };
    const uint8_t black[4] = { 0, 0, 0, 0 };
    const vkm::VkmImageData base = makeCheckerboard(4, 4, white, black);

    std::vector<vkm::VkmImageData> levels;
    vkm::vkmBuildMipChain(base, /*srgb=*/true, &levels);
    REQUIRE(levels.size() >= 1);

    const uint8_t* texel = levels[0]._pixels.data();
    for (uint32_t c = 0; c < 3; ++c)
    {
        CHECK(static_cast<int>(texel[c]) == doctest::Approx(188).epsilon(0.02));
        // Stated separately so a regression reads as "averaged in the wrong space" rather than as
        // an off-by-a-few rounding difference.
        CHECK(static_cast<int>(texel[c]) > 160);
    }

    SUBCASE("alpha is linear even in an sRGB texture")
    {
        // Half 255, half 0: the linear mean is 128. Running alpha through the sRGB curve would
        // give ~188 instead, the same number the colour channels correctly produce -- so this
        // fails loudly if the channel split is dropped.
        CHECK(static_cast<int>(texel[3]) == doctest::Approx(128).epsilon(0.02));
    }
}

TEST_CASE("vkmBuildMipChain averages linear data arithmetically")
{
    // The same checkerboard with srgb=false must give the plain mean, 128 -- the metallic-roughness
    // and normal-map case, where applying a colour curve would corrupt the data.
    const uint8_t white[4] = { 255, 255, 255, 255 };
    const uint8_t black[4] = { 0, 0, 0, 255 };
    const vkm::VkmImageData base = makeCheckerboard(4, 4, white, black);

    std::vector<vkm::VkmImageData> levels;
    vkm::vkmBuildMipChain(base, /*srgb=*/false, &levels);
    REQUIRE(levels.size() >= 1);

    const uint8_t* texel = levels[0]._pixels.data();
    for (uint32_t c = 0; c < 3; ++c)
    {
        CHECK(static_cast<int>(texel[c]) == doctest::Approx(128).epsilon(0.02));
    }
}

TEST_CASE("vkmBuildMipChain handles degenerate and odd sizes without running off the source")
{
    std::vector<vkm::VkmImageData> levels;
    const uint8_t opaque[4] = { 10, 20, 30, 255 };

    SUBCASE("a 1x1 image has no levels below it")
    {
        const vkm::VkmImageData base = makeCheckerboard(1, 1, opaque, opaque);
        vkm::vkmBuildMipChain(base, /*srgb=*/true, &levels);
        CHECK(levels.empty());
    }

    SUBCASE("an empty image produces nothing rather than dividing by zero")
    {
        vkm::VkmImageData empty;
        vkm::vkmBuildMipChain(empty, /*srgb=*/true, &levels);
        CHECK(levels.empty());
    }

    SUBCASE("odd dimensions still reach 1x1 with every level fully sized")
    {
        const vkm::VkmImageData base = makeCheckerboard(5, 3, opaque, opaque);
        vkm::vkmBuildMipChain(base, /*srgb=*/false, &levels);
        REQUIRE(levels.size() == vkm::vkmMipLevelCount(5, 3) - 1);
        for (const vkm::VkmImageData& level : levels)
        {
            CHECK(level._width >= 1);
            CHECK(level._height >= 1);
            CHECK(level._pixels.size() == static_cast<size_t>(level._width) * level._height * 4);
        }
        CHECK(levels.back()._width == 1);
        CHECK(levels.back()._height == 1);
    }
}
