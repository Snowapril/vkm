// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    /*
    * @brief CPU-side pixels of a decoded image, ready to hand to
    * VkmDriverBase::uploadToTexture().
    *
    * Always 8-bit RGBA, tightly packed, top-left origin -- the layout VkmFormat::R8G8B8A8_*
    * expects. Decoding to a fixed 4-channel format (rather than the file's own channel
    * count) is what keeps the upload path free of row-pitch and swizzle special cases.
    */
    struct VkmImageData
    {
        std::vector<uint8_t> _pixels;
        uint32_t _width = 0;
        uint32_t _height = 0;

        inline uint64_t getByteSize() const { return _pixels.size(); }
    };

    /*
    * @brief Decode an image file (PNG, JPEG, TGA, BMP, ... -- whatever stb_image supports)
    * into 8-bit RGBA pixels.
    *
    * Exception-free by design (emscripten builds compile without -fexceptions), matching
    * importGltfModel: every failure is reported through the return value and `outError`.
    *
    * @return true on success; false leaves `outImage` untouched and fills `outError`.
    */
    bool loadImageFromFile(const std::string& filePath, VkmImageData* outImage, std::string* outError);

    // Mip levels a texture of this size has, counting the base: floor(log2(max)) + 1.
    uint32_t vkmMipLevelCount(uint32_t width, uint32_t height);

    /*
    * @brief Builds the mip chain below `base` by repeated 2x2 box filtering.
    *
    * @details `outLevels` receives levels 1..N-1 -- the base is the caller's and is not copied.
    * Level k has dimensions max(1, w >> k) x max(1, h >> k), which is what every backend's
    * copyBufferToTexture/writeRegion computes for that level.
    *
    * **`srgb` is not cosmetic.** An sRGB texture stores gamma-encoded values, and averaging those
    * directly is averaging the wrong quantity: half-black/half-white averages to 128, where the
    * correct answer is the encoding of linear 0.5, which is about 188. Getting this backwards makes
    * every mip chain visibly too dark, worst at the coarsest levels, and it is exactly the bug a
    * solid-colour test cannot see. Pass true for base colour and emissive, false for the linear
    * data (metallic-roughness, normal maps).
    *
    * Alpha is always averaged linearly -- sRGB encodes only the colour channels.
    */
    void vkmBuildMipChain(const VkmImageData& base, bool srgb, std::vector<VkmImageData>* outLevels);
} // namespace vkm
