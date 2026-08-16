// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>

namespace vkm
{
    /*
    * @brief Puts an RGBA8 image into the OS clipboard.
    * @details Implemented on macOS (NSPasteboard, PNG) and Windows (CF_DIB). Only those two
    * platforms provide a definition, so callers must be gated to them at compile time.
    * @param width Image width in pixels.
    * @param height Image height in pixels.
    * @param rgba8 Tightly packed RGBA8 pixels, top-down rows, width * height * 4 bytes.
    * @return False when any OS clipboard call fails.
    */
    bool vkmSetClipboardImage(uint32_t width, uint32_t height, const uint8_t* rgba8);
} // namespace vkm
