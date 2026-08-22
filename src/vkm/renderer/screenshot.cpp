// Copyright (c) 2026 Snowapril

#include <vkm/renderer/screenshot.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vkm
{
    namespace
    {
        // IEEE half -> float. Small enough to keep here rather than pull in a dependency, and this
        // is the only place in the engine that needs it.
        float halfToFloat(uint16_t half)
        {
            const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
            const uint32_t exponent = (half >> 10) & 0x1Fu;
            const uint32_t mantissa = half & 0x3FFu;

            uint32_t bits = 0;
            if (exponent == 0)
            {
                // Zero or subnormal. Subnormals are rare enough in a render target that the
                // renormalizing loop below costs nothing in practice.
                if (mantissa != 0)
                {
                    uint32_t e = 127 - 15 + 1;
                    uint32_t m = mantissa;
                    while ((m & 0x400u) == 0)
                    {
                        m <<= 1;
                        --e;
                    }
                    bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
                }
                else
                {
                    bits = sign;
                }
            }
            else if (exponent == 0x1Fu)
            {
                bits = sign | 0x7F800000u | (mantissa << 13); // inf / NaN
            }
            else
            {
                bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
            }

            float result = 0.0f;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
        }

        uint8_t toUnorm8(float value)
        {
            // NaN sorts as neither, so clamp explicitly rather than relying on std::clamp.
            if (!(value > 0.0f))
            {
                return 0;
            }
            return static_cast<uint8_t>(std::min(value, 1.0f) * 255.0f + 0.5f);
        }
    } // namespace

    bool vkmWriteTexturePng(VkmDriverBase* driver, VkmResourceHandle textureHandle, const std::string& path)
    {
        VKM_ASSERT(driver != nullptr, "vkmWriteTexturePng requires a driver");

        VkmTexture* texture = driver->getRenderResourcePool()->getResource<VkmTexture>(textureHandle);
        if (texture == nullptr)
        {
            VKM_DEBUG_ERROR("vkmWriteTexturePng: no such texture");
            return false;
        }
        const VkmFormat format = texture->getTextureInfo()._format;

        const VkmTextureReadbackResult readback = driver->readbackTexture(textureHandle);
        if (readback.pixels.empty() || readback.width == 0 || readback.height == 0)
        {
            VKM_DEBUG_ERROR("vkmWriteTexturePng: readback produced no pixels");
            return false;
        }

        const size_t texelCount = static_cast<size_t>(readback.width) * readback.height;
        std::vector<uint8_t> rgba(texelCount * 4, 0);

        switch (format)
        {
            case VkmFormat::R8G8B8A8_UNORM:
            case VkmFormat::R8G8B8A8_SRGB:
                for (size_t i = 0; i < texelCount; ++i)
                {
                    std::memcpy(&rgba[i * 4], &readback.pixels[i * readback.channels], 4);
                }
                break;
            case VkmFormat::BGRA8_UNORM:
            case VkmFormat::BGRA8_SRGB:
                for (size_t i = 0; i < texelCount; ++i)
                {
                    const uint8_t* src = &readback.pixels[i * readback.channels];
                    rgba[i * 4 + 0] = src[2];
                    rgba[i * 4 + 1] = src[1];
                    rgba[i * 4 + 2] = src[0];
                    rgba[i * 4 + 3] = src[3];
                }
                break;
            case VkmFormat::R16G16B16A16_SFLOAT:
                for (size_t i = 0; i < texelCount; ++i)
                {
                    const uint8_t* src = &readback.pixels[i * readback.channels];
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        uint16_t half = 0;
                        std::memcpy(&half, src + c * sizeof(uint16_t), sizeof(half));
                        rgba[i * 4 + c] = toUnorm8(halfToFloat(half));
                    }
                }
                break;
            default:
                VKM_DEBUG_ERROR("vkmWriteTexturePng: unsupported texture format");
                return false;
        }

        const int stride = static_cast<int>(readback.width) * 4;
        if (stbi_write_png(path.c_str(), static_cast<int>(readback.width), static_cast<int>(readback.height),
                           4, rgba.data(), stride) == 0)
        {
            VKM_DEBUG_ERROR(("vkmWriteTexturePng: failed to write " + path).c_str());
            return false;
        }
        VKM_DEBUG_INFO(("Wrote screenshot " + path).c_str());
        return true;
    }
} // namespace vkm
