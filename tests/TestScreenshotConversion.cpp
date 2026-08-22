// Copyright (c) 2025 Snowapril

#include <doctest/doctest.h>

#include <vkm/renderer/screenshot.h>

#include "TestHalfFloatShared.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

using vkm::VkmFormat;
using vkm::VkmTextureReadbackResult;

namespace
{
    // A readback with `channels` bytes per texel, `texelCount` texels, filled from one 4-byte
    // pattern per texel so tests can assert on identity/swizzle without hand-writing every byte.
    VkmTextureReadbackResult makeReadback(uint32_t width, uint32_t height, uint32_t channels,
                                          const std::vector<uint8_t>& texelBytes)
    {
        VkmTextureReadbackResult readback;
        readback.width = width;
        readback.height = height;
        readback.channels = channels;
        readback.pixels.resize(static_cast<size_t>(width) * height * channels);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
        {
            std::memcpy(&readback.pixels[i * channels], texelBytes.data(), channels);
        }
        return readback;
    }
} // namespace

TEST_CASE("vkmConvertReadbackToRgba8 - R8G8B8A8_UNORM passes bytes through unchanged")
{
    const std::vector<uint8_t> texel = { 10, 20, 30, 40 };
    const VkmTextureReadbackResult readback = makeReadback(2, 2, 4, texel);

    std::vector<uint8_t> rgba;
    REQUIRE(vkm::vkmConvertReadbackToRgba8(readback, VkmFormat::R8G8B8A8_UNORM, rgba));
    REQUIRE(rgba.size() == 2u * 2u * 4u);
    for (size_t texelIndex = 0; texelIndex < 4; ++texelIndex)
    {
        CHECK(rgba[texelIndex * 4 + 0] == texel[0]);
        CHECK(rgba[texelIndex * 4 + 1] == texel[1]);
        CHECK(rgba[texelIndex * 4 + 2] == texel[2]);
        CHECK(rgba[texelIndex * 4 + 3] == texel[3]);
    }
}

TEST_CASE("vkmConvertReadbackToRgba8 - BGRA8_UNORM swizzles to RGBA, alpha preserved")
{
    const std::vector<uint8_t> texel = { 1, 2, 3, 4 }; // B, G, R, A
    const VkmTextureReadbackResult readback = makeReadback(1, 1, 4, texel);

    std::vector<uint8_t> rgba;
    REQUIRE(vkm::vkmConvertReadbackToRgba8(readback, VkmFormat::BGRA8_UNORM, rgba));
    REQUIRE(rgba.size() == 4);
    CHECK(rgba[0] == 3); // R
    CHECK(rgba[1] == 2); // G
    CHECK(rgba[2] == 1); // B
    CHECK(rgba[3] == 4); // A
}

TEST_CASE("vkmConvertReadbackToRgba8 - R16G16B16A16_SFLOAT tone-maps by clamping")
{
    std::vector<uint8_t> texel(8, 0);
    const uint16_t half0 = vkmtest::encodeHalf(0.0f);
    const uint16_t halfHalf = vkmtest::encodeHalf(0.5f);
    const uint16_t half1 = vkmtest::encodeHalf(1.0f);
    const uint16_t halfOver = vkmtest::encodeHalf(2.0f);
    std::memcpy(&texel[0], &half0, 2);
    std::memcpy(&texel[2], &halfHalf, 2);
    std::memcpy(&texel[4], &half1, 2);
    std::memcpy(&texel[6], &halfOver, 2);

    const VkmTextureReadbackResult readback = makeReadback(1, 1, 8, texel);

    std::vector<uint8_t> rgba;
    REQUIRE(vkm::vkmConvertReadbackToRgba8(readback, VkmFormat::R16G16B16A16_SFLOAT, rgba));
    REQUIRE(rgba.size() == 4);
    CHECK(rgba[0] == 0);
    CHECK(static_cast<int>(rgba[1]) == doctest::Approx(128).epsilon(0.02));
    CHECK(rgba[2] == 255);
    CHECK(rgba[3] == 255); // clamped: >1.0 saturates to 255
}

TEST_CASE("vkmConvertReadbackToRgba8 - an empty readback fails")
{
    VkmTextureReadbackResult readback;
    std::vector<uint8_t> rgba;
    CHECK_FALSE(vkm::vkmConvertReadbackToRgba8(readback, VkmFormat::R8G8B8A8_UNORM, rgba));
}

TEST_CASE("vkmConvertReadbackToRgba8 - an unsupported format fails")
{
    const std::vector<uint8_t> texel(4, 0);
    const VkmTextureReadbackResult readback = makeReadback(1, 1, 4, texel);

    std::vector<uint8_t> rgba;
    CHECK_FALSE(vkm::vkmConvertReadbackToRgba8(readback, VkmFormat::D32_SFLOAT, rgba));
}
