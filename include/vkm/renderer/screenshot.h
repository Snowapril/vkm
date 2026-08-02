// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <string>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief Reads a colour texture back and writes it to `path` as a PNG.
    *
    * @details The point is verification without a display: a sample that renders into an offscreen
    * target can produce a file to look at (or diff) on a machine where nobody is watching the
    * window, which is the only way some of this can be checked at all.
    *
    * Blocks -- it goes through VkmDriverBase::readbackTexture, which submits and waits. Call it
    * between frames, not inside one, and only when a screenshot was actually asked for.
    *
    * Handles the formats a presentable or HDR target actually uses: 8-bit RGBA/BGRA (swizzling the
    * latter) and RGBA16F, which is tone-mapped by clamping rather than by any curve -- an HDR
    * target dumped straight to PNG is for inspecting values, not for judging the final image.
    * Anything else is refused rather than guessed at.
    *
    * The backbuffer itself is deliberately not the input: Metal keeps `framebufferOnly = YES` on
    * the drawable, so it cannot be read back at all (see TODO.md). Render the final pass into an
    * owned target as well and hand that in.
    */
    bool vkmWriteTexturePng(VkmDriverBase* driver, VkmResourceHandle textureHandle, const std::string& path);
} // namespace vkm
