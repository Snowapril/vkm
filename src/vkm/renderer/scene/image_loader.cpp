// Copyright (c) 2025 Snowapril

#include <vkm/renderer/scene/image_loader.h>

#include <vkm/base/common.h>

#include <stb_image.h>

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
} // namespace vkm
