// Copyright (c) 2026 Snowapril

#include <vkm/renderer/scene/image_loader.h>

#include <vkm/base/common.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace vkm
{
    namespace
    {
        // Every texture the engine samples is RGBA8 (see VkmFormat), so the decoder is asked
        // for 4 channels regardless of what the file holds -- stb_image expands or drops
        // channels for us, which is cheaper than teaching the upload path about pitches.
        constexpr int kRequestedChannels = 4;
    } // namespace

    bool loadImageFromFile(const std::string& filePath, VkmImageData* outImage, std::string* outError)
    {
        VKM_ASSERT(outImage != nullptr, "loadImageFromFile requires an output image");

        int width = 0;
        int height = 0;
        int channelsInFile = 0;
        stbi_uc* decoded = stbi_load(filePath.c_str(), &width, &height, &channelsInFile, kRequestedChannels);
        if (decoded == nullptr)
        {
            if (outError != nullptr)
            {
                const char* reason = stbi_failure_reason();
                *outError = "Failed to decode '" + filePath + "': " + (reason != nullptr ? reason : "unknown error");
            }
            return false;
        }

        // stb_image reports the file's own dimensions as int; a negative or zero extent would
        // mean a corrupt decode that still returned pixels.
        if (width <= 0 || height <= 0)
        {
            stbi_image_free(decoded);
            if (outError != nullptr)
            {
                *outError = "Decoded '" + filePath + "' has a degenerate extent";
            }
            return false;
        }

        const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * kRequestedChannels;
        outImage->_pixels.assign(decoded, decoded + byteSize);
        outImage->_width = static_cast<uint32_t>(width);
        outImage->_height = static_cast<uint32_t>(height);

        stbi_image_free(decoded);
        return true;
    }

namespace
{
    // sRGB <-> linear on the 8-bit encoding, per the sRGB spec's piecewise curve. Tabulated on
    // the decode side because a mip build decodes every texel of every level; the encode side is
    // called once per output texel and stays closed-form.
    const std::array<float, 256>& srgbToLinearTable()
    {
        static const std::array<float, 256> table = [] {
            std::array<float, 256> t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                const float c = static_cast<float>(i) / 255.0f;
                t[i] = (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            return t;
        }();
        return table;
    }

    uint8_t linearToSrgb8(float linear)
    {
        const float c = (linear <= 0.0031308f) ? (linear * 12.92f)
                                               : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
        return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
    }
} // namespace

uint32_t vkmMipLevelCount(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    uint32_t extent = std::max(width, height);
    while (extent > 1)
    {
        extent >>= 1;
        ++levels;
    }
    return levels;
}

void vkmBuildMipChain(const VkmImageData& base, bool srgb, std::vector<VkmImageData>* outLevels)
{
    VKM_ASSERT(outLevels != nullptr, "vkmBuildMipChain needs somewhere to put the levels");
    outLevels->clear();
    if (base._width == 0 || base._height == 0)
    {
        return;
    }

    constexpr uint32_t kChannels = 4;
    const std::array<float, 256>& toLinear = srgbToLinearTable();

    const VkmImageData* source = &base;
    const uint32_t levelCount = vkmMipLevelCount(base._width, base._height);
    // Reserved up front so `source`, which points into the vector, survives every push_back.
    outLevels->reserve(levelCount - 1u);

    for (uint32_t level = 1; level < levelCount; ++level)
    {
        const uint32_t dstWidth = std::max(1u, base._width >> level);
        const uint32_t dstHeight = std::max(1u, base._height >> level);

        VkmImageData next;
        next._width = dstWidth;
        next._height = dstHeight;
        next._pixels.resize(static_cast<size_t>(dstWidth) * dstHeight * kChannels);

        // A 2x2 box over the source. When the source extent is odd the last row/column is dropped
        // rather than weighted in: the sample positions no longer line up, and a correct answer
        // there needs a proper resampling kernel rather than a box. Acceptable for a mip chain and
        // recorded in TODO.md.
        for (uint32_t y = 0; y < dstHeight; ++y)
        {
            for (uint32_t x = 0; x < dstWidth; ++x)
            {
                const uint32_t sx0 = std::min(x * 2u, source->_width - 1u);
                const uint32_t sy0 = std::min(y * 2u, source->_height - 1u);
                const uint32_t sx1 = std::min(sx0 + 1u, source->_width - 1u);
                const uint32_t sy1 = std::min(sy0 + 1u, source->_height - 1u);

                const size_t taps[4] = {
                    (static_cast<size_t>(sy0) * source->_width + sx0) * kChannels,
                    (static_cast<size_t>(sy0) * source->_width + sx1) * kChannels,
                    (static_cast<size_t>(sy1) * source->_width + sx0) * kChannels,
                    (static_cast<size_t>(sy1) * source->_width + sx1) * kChannels,
                };

                uint8_t* dst = next._pixels.data() + (static_cast<size_t>(y) * dstWidth + x) * kChannels;
                for (uint32_t c = 0; c < kChannels; ++c)
                {
                    // Alpha is linear even in an sRGB texture, so only rgb goes through the curve.
                    const bool encoded = srgb && c < 3;
                    float sum = 0.0f;
                    for (size_t tap : taps)
                    {
                        const uint8_t value = source->_pixels[tap + c];
                        sum += encoded ? toLinear[value] : static_cast<float>(value);
                    }
                    const float average = sum * 0.25f;
                    dst[c] = encoded ? linearToSrgb8(average)
                                     : static_cast<uint8_t>(std::lround(std::clamp(average, 0.0f, 255.0f)));
                }
            }
        }

        outLevels->push_back(std::move(next));
        source = &outLevels->back();
    }
}

} // namespace vkm
