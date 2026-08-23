// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief Reads a colour texture back and writes it as a PNG. For verification without a display.
    * @details Blocks: it goes through VkmDriverBase::readbackTexture, which submits and waits. Call
    * it between frames, not inside one.
    * Handles the formats a presentable or HDR target uses -- 8-bit RGBA/BGRA, swizzling the latter,
    * and RGBA16F, tone-mapped by clamping rather than by any curve. Anything else is refused.
    * @param driver Driver owning the texture.
    * @param textureHandle Texture to read back, which must be readable as a transfer source (the
    * engine's main-window back buffer qualifies; the dedicated ImGui window's drawable does not).
    * @param path Destination file.
    * @return False when the format is unsupported or the file could not be written.
    */
    bool vkmWriteTexturePng(VkmDriverBase* driver, VkmResourceHandle textureHandle, const std::string& path);

    /*
    * @brief Converts a texture readback into tightly packed RGBA8 pixels, top-down rows.
    * @details Handles the formats a presentable or HDR target uses -- 8-bit RGBA/BGRA, swizzling
    * the latter, and RGBA16F, tone-mapped by clamping rather than by any curve. Anything else is
    * refused.
    * @param readback Pixels produced by VkmDriverBase::readbackTexture.
    * @param format Format of the texture the readback came from.
    * @param outRgba8 Receives readback.width * readback.height * 4 bytes on success.
    * @return False when the readback is empty or the format is unsupported.
    */
    bool vkmConvertReadbackToRgba8(const VkmTextureReadbackResult& readback, VkmFormat format,
                                   std::vector<uint8_t>& outRgba8);
} // namespace vkm
