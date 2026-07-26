// Copyright (c) 2025 Snowapril

#include <vkm/renderer/scene/vertex_layout.h>

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>

namespace vkm
{
    namespace
    {
        VkmVertexLayout makeLayout(std::initializer_list<VkmVertexAttribute> attributes,
                                   uint32_t stride,
                                   VkmVertexLayoutPreset preset)
        {
            VkmVertexLayout layout;
            layout._attributeCount = 0;
            for (const VkmVertexAttribute& attribute : attributes)
            {
                layout._attributes[layout._attributeCount++] = attribute;
            }
            layout._stride = stride;
            layout._preset = preset;
            return layout;
        }

        /*
        * @brief The preset table.
        *
        * StandardPBR reproduces the byte layout the engine used before vertex layouts existed
        * (position@0, normal@16, uv0@32, tangent@48, stride 64): the padding is the std430-like
        * layout DXC emits for storage-buffer structs on -spirv targets, and keeping it identical
        * means existing shader permutations and captured .gputrace files stay valid.
        *
        * PositionOnly pads to 16 so a position stays float4-aligned. Compact pads to 32 for the
        * same reason; its quantized attributes occupy the first 24 bytes.
        */
        const std::array<VkmVertexLayout, static_cast<size_t>(VkmVertexLayoutPreset::Count)>& getPresetTable()
        {
            static const std::array<VkmVertexLayout, static_cast<size_t>(VkmVertexLayoutPreset::Count)> table = {
                makeLayout({ { VkmVertexSemantic::Position, VkmVertexAttributeFormat::Float32x3, 0 } },
                           16, VkmVertexLayoutPreset::PositionOnly),
                makeLayout({ { VkmVertexSemantic::Position, VkmVertexAttributeFormat::Float32x3, 0 },
                             { VkmVertexSemantic::Normal, VkmVertexAttributeFormat::Float32x3, 16 },
                             { VkmVertexSemantic::UV0, VkmVertexAttributeFormat::Float32x2, 32 },
                             { VkmVertexSemantic::Tangent, VkmVertexAttributeFormat::Float32x4, 48 } },
                           64, VkmVertexLayoutPreset::StandardPBR),
                makeLayout({ { VkmVertexSemantic::Position, VkmVertexAttributeFormat::Float32x3, 0 },
                             { VkmVertexSemantic::Normal, VkmVertexAttributeFormat::Snorm8x4, 12 },
                             { VkmVertexSemantic::UV0, VkmVertexAttributeFormat::Float16x2, 16 },
                             { VkmVertexSemantic::Tangent, VkmVertexAttributeFormat::Snorm8x4, 20 } },
                           32, VkmVertexLayoutPreset::Compact),
            };
            return table;
        }

        void decode(const uint8_t* source, VkmVertexAttributeFormat format, float* out)
        {
            switch (format)
            {
                case VkmVertexAttributeFormat::Float32x2:
                case VkmVertexAttributeFormat::Float32x3:
                case VkmVertexAttributeFormat::Float32x4:
                    std::memcpy(out, source, vkmGetVertexAttributeFormatSize(format));
                    return;
                case VkmVertexAttributeFormat::Float16x2:
                {
                    uint16_t halves[2];
                    std::memcpy(halves, source, sizeof(halves));
                    out[0] = meshopt_dequantizeHalf(halves[0]);
                    out[1] = meshopt_dequantizeHalf(halves[1]);
                    return;
                }
                case VkmVertexAttributeFormat::Snorm8x4:
                {
                    int8_t packed[4];
                    std::memcpy(packed, source, sizeof(packed));
                    for (uint32_t i = 0; i < 4; ++i)
                    {
                        // The inverse of meshopt_quantizeSnorm(v, 8): 127 is the scale that
                        // function uses, and clamping keeps -128 (unreachable by encoding, but
                        // possible in externally authored data) at exactly -1.
                        out[i] = std::max(static_cast<float>(packed[i]) / 127.0f, -1.0f);
                    }
                    return;
                }
                default:
                    VKM_ASSERT(false, "Unhandled VkmVertexAttributeFormat in decode");
                    return;
            }
        }

        void encode(uint8_t* destination, VkmVertexAttributeFormat format, const float* in)
        {
            switch (format)
            {
                case VkmVertexAttributeFormat::Float32x2:
                case VkmVertexAttributeFormat::Float32x3:
                case VkmVertexAttributeFormat::Float32x4:
                    std::memcpy(destination, in, vkmGetVertexAttributeFormatSize(format));
                    return;
                case VkmVertexAttributeFormat::Float16x2:
                {
                    const uint16_t halves[2] = { meshopt_quantizeHalf(in[0]), meshopt_quantizeHalf(in[1]) };
                    std::memcpy(destination, halves, sizeof(halves));
                    return;
                }
                case VkmVertexAttributeFormat::Snorm8x4:
                {
                    int8_t packed[4];
                    for (uint32_t i = 0; i < 4; ++i)
                    {
                        packed[i] = static_cast<int8_t>(meshopt_quantizeSnorm(in[i], 8));
                    }
                    std::memcpy(destination, packed, sizeof(packed));
                    return;
                }
                default:
                    VKM_ASSERT(false, "Unhandled VkmVertexAttributeFormat in encode");
                    return;
            }
        }
    } // namespace

    uint32_t vkmGetVertexAttributeFormatSize(VkmVertexAttributeFormat format)
    {
        switch (format)
        {
            case VkmVertexAttributeFormat::Float32x2: return 8;
            case VkmVertexAttributeFormat::Float32x3: return 12;
            case VkmVertexAttributeFormat::Float32x4: return 16;
            case VkmVertexAttributeFormat::Float16x2: return 4;
            case VkmVertexAttributeFormat::Snorm8x4:  return 4;
            default:
                VKM_ASSERT(false, "Unhandled VkmVertexAttributeFormat");
                return 0;
        }
    }

    uint32_t vkmGetVertexAttributeFormatComponentCount(VkmVertexAttributeFormat format)
    {
        switch (format)
        {
            case VkmVertexAttributeFormat::Float32x2: return 2;
            case VkmVertexAttributeFormat::Float32x3: return 3;
            case VkmVertexAttributeFormat::Float32x4: return 4;
            case VkmVertexAttributeFormat::Float16x2: return 2;
            case VkmVertexAttributeFormat::Snorm8x4:  return 4;
            default:
                VKM_ASSERT(false, "Unhandled VkmVertexAttributeFormat");
                return 0;
        }
    }

    const VkmVertexLayout& vkmGetVertexLayoutPreset(VkmVertexLayoutPreset preset)
    {
        VKM_ASSERT(preset < VkmVertexLayoutPreset::Count, "Unknown VkmVertexLayoutPreset");
        return getPresetTable()[static_cast<size_t>(preset)];
    }

    const char* vkmVertexLayoutPresetName(VkmVertexLayoutPreset preset)
    {
        switch (preset)
        {
            case VkmVertexLayoutPreset::PositionOnly: return "position_only";
            case VkmVertexLayoutPreset::StandardPBR:  return "standard_pbr";
            case VkmVertexLayoutPreset::Compact:      return "compact";
            default:
                VKM_ASSERT(false, "Unknown VkmVertexLayoutPreset");
                return "";
        }
    }

    const VkmVertexAttribute* vkmFindVertexAttribute(const VkmVertexLayout& layout, VkmVertexSemantic semantic)
    {
        for (uint32_t i = 0; i < layout._attributeCount; ++i)
        {
            if (layout._attributes[i]._semantic == semantic)
            {
                return &layout._attributes[i];
            }
        }
        return nullptr;
    }

    void vkmReadVertexAttribute(const uint8_t* vertex,
                                const VkmVertexLayout& layout,
                                VkmVertexSemantic semantic,
                                float* out,
                                uint32_t componentCount)
    {
        const VkmVertexAttribute* attribute = vkmFindVertexAttribute(layout, semantic);
        if (attribute == nullptr)
        {
            for (uint32_t i = 0; i < componentCount; ++i)
            {
                out[i] = 0.0f;
            }
            return;
        }

        // Decode into a full-width scratch so a caller asking for fewer components than the
        // attribute stores never sees a partially decoded packed format.
        float scratch[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        decode(vertex + attribute->_offset, attribute->_format, scratch);

        const uint32_t stored = vkmGetVertexAttributeFormatComponentCount(attribute->_format);
        for (uint32_t i = 0; i < componentCount; ++i)
        {
            out[i] = i < stored ? scratch[i] : 0.0f;
        }
    }

    void vkmWriteVertexAttribute(uint8_t* vertex,
                                 const VkmVertexLayout& layout,
                                 VkmVertexSemantic semantic,
                                 const float* in,
                                 uint32_t componentCount)
    {
        const VkmVertexAttribute* attribute = vkmFindVertexAttribute(layout, semantic);
        if (attribute == nullptr)
        {
            return;
        }

        const uint32_t stored = vkmGetVertexAttributeFormatComponentCount(attribute->_format);
        float scratch[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (uint32_t i = 0; i < stored; ++i)
        {
            scratch[i] = i < componentCount ? in[i] : 0.0f;
        }
        encode(vertex + attribute->_offset, attribute->_format, scratch);
    }
} // namespace vkm
