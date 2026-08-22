// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <string>

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
    * @param textureHandle Texture to read back. Not the backbuffer: Metal keeps
    * `framebufferOnly = YES` on the drawable, so render the final pass into an owned target too and
    * hand that in.
    * @param path Destination file.
    * @return False when the format is unsupported or the file could not be written.
    */
    bool vkmWriteTexturePng(VkmDriverBase* driver, VkmResourceHandle textureHandle, const std::string& path);
} // namespace vkm
