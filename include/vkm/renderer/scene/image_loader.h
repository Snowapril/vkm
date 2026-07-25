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
} // namespace vkm
