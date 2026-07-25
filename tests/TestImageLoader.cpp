#include <doctest/doctest.h>

#include <vkm/renderer/scene/image_loader.h>

#include <string>

namespace
{
// Solid-color 64x64 RGBA PNGs committed under resources/tests/.
const std::string kRedImagePath = std::string(RESOURCES_DIR) + "tests/reference_red_64x64.png";
const std::string kGreenImagePath = std::string(RESOURCES_DIR) + "tests/reference_green_64x64.png";

constexpr uint32_t kExpectedExtent = 64;
constexpr size_t kChannels = 4;
} // namespace

TEST_CASE("loadImageFromFile decodes a PNG to tightly-packed RGBA8")
{
    vkm::VkmImageData image;
    std::string error;
    REQUIRE_MESSAGE(vkm::loadImageFromFile(kRedImagePath, &image, &error), error);

    CHECK(image._width == kExpectedExtent);
    CHECK(image._height == kExpectedExtent);
    // Tightly packed and 4-channel regardless of the file's own channel count, which is what
    // lets uploadToTexture treat the pixels as one flat blob.
    CHECK(image.getByteSize() == static_cast<uint64_t>(kExpectedExtent) * kExpectedExtent * kChannels);

    SUBCASE("channels are in RGBA order")
    {
        CHECK(image._pixels[0] == 255); // R
        CHECK(image._pixels[1] == 0);   // G
        CHECK(image._pixels[2] == 0);   // B
        CHECK(image._pixels[3] == 255); // A
    }

    SUBCASE("the whole image decoded, not just the first row")
    {
        const size_t lastTexel = image._pixels.size() - kChannels;
        CHECK(image._pixels[lastTexel + 0] == 255);
        CHECK(image._pixels[lastTexel + 1] == 0);
        CHECK(image._pixels[lastTexel + 2] == 0);
    }
}

TEST_CASE("loadImageFromFile distinguishes different images")
{
    vkm::VkmImageData green;
    std::string error;
    REQUIRE_MESSAGE(vkm::loadImageFromFile(kGreenImagePath, &green, &error), error);

    CHECK(green._pixels[0] == 0);
    CHECK(green._pixels[1] == 255);
    CHECK(green._pixels[2] == 0);
}

TEST_CASE("loadImageFromFile reports an error instead of throwing")
{
    vkm::VkmImageData image;
    std::string error;

    SUBCASE("missing file")
    {
        CHECK_FALSE(vkm::loadImageFromFile(std::string(RESOURCES_DIR) + "tests/does_not_exist.png", &image, &error));
        CHECK_FALSE(error.empty());
        CHECK(image._width == 0); // untouched on failure
    }

    SUBCASE("file that is not an image")
    {
        CHECK_FALSE(vkm::loadImageFromFile(std::string(RESOURCES_DIR) + "tests/pso_minimal.json", &image, &error));
        CHECK_FALSE(error.empty());
    }
}
