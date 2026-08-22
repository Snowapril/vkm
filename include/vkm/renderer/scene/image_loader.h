// Copyright (c) 2026 Snowapril

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
    * @brief Decode an image file into 8-bit RGBA pixels: PNG, JPEG, TGA, BMP, whatever stb_image
    * supports.
    * @details Exception-free, emscripten builds compiling without -fexceptions, so every failure is
    * reported through the return value.
    * @param filePath File to decode.
    * @param outImage Receives the decoded pixels. Untouched on failure.
    * @param outError Receives the failure reason.
    * @return False when the file could not be read or decoded.
    */
    bool loadImageFromFile(const std::string& filePath, VkmImageData* outImage, std::string* outError);

    // Mip levels a texture of this size has, counting the base: floor(log2(max)) + 1.
    uint32_t vkmMipLevelCount(uint32_t width, uint32_t height);

    /*
    * @brief Builds the mip chain below a base level by repeated 2x2 box filtering.
    * @details Level k has dimensions max(1, w >> k) x max(1, h >> k), which is what every backend's
    * copyBufferToTexture/writeRegion computes for that level.
    * @param base Level 0. Not copied into the output.
    * @param srgb Whether the colour channels are gamma-encoded. Not cosmetic: averaging encoded
    * values averages the wrong quantity -- half-black/half-white averages to 128 where the correct
    * answer is the encoding of linear 0.5, about 188 -- which makes the whole chain too dark. True
    * for base colour and emissive, false for metallic-roughness and normal maps. Alpha is always
    * averaged linearly, sRGB encoding only the colour channels.
    * @param outLevels Receives levels 1..N-1.
    */
    void vkmBuildMipChain(const VkmImageData& base, bool srgb, std::vector<VkmImageData>* outLevels);
} // namespace vkm
