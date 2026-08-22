// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>

#include <cstdint>

namespace vkm
{
    enum class VkmVertexSemantic : uint8_t
    {
        Position = 0,
        Normal = 1,
        Tangent = 2,
        UV0 = 3,
        Count = 4,
    };

    /*
    * @brief Storage format of one vertex attribute.
    *
    * Deliberately separate from VkmFormat, which is a pixel/attachment format enum wired into
    * every backend's texture-format converter: none of these are valid there, and the packed
    * ones have no texture equivalent at all.
    */
    enum class VkmVertexAttributeFormat : uint8_t
    {
        Float32x2 = 0,
        Float32x3 = 1,
        Float32x4 = 2,
        Float16x2 = 3, // half2, via meshopt_quantizeHalf / meshopt_dequantizeHalf
        Snorm8x4 = 4,  // packed normal/tangent, via meshopt_quantizeSnorm
        Count = 5,
    };

    uint32_t vkmGetVertexAttributeFormatSize(VkmVertexAttributeFormat format);
    uint32_t vkmGetVertexAttributeFormatComponentCount(VkmVertexAttributeFormat format);

    struct VkmVertexAttribute
    {
        VkmVertexSemantic _semantic = VkmVertexSemantic::Position;
        VkmVertexAttributeFormat _format = VkmVertexAttributeFormat::Float32x3;
        uint32_t _offset = 0; // byte offset within VkmVertexLayout::_stride
    };

    /*
    * @brief Which engine-provided layout a mesh's vertex bytes are in.
    *
    * One shader permutation exists per preset (the VKM_VERTEX_LAYOUT_* defines supplied by the
    * PSO JSON "options"), so this is a closed set rather than a free-form descriptor: a layout
    * the shaders have no permutation for cannot be drawn.
    */
    enum class VkmVertexLayoutPreset : uint8_t
    {
        PositionOnly = 0, // 16 B
        StandardPBR = 1,  // 64 B -- byte-identical to the vertex layout the engine used before
        Compact = 2,      // 32 B -- quantized normal/tangent/uv
        Count = 3,
    };

    /*
    * @brief Interleaved vertex layout description.
    *
    * `_stride` is always a multiple of 4 because geometry pools are addressed in u32 words on
    * every backend (see VkmSceneGeometryPool and the DATA_WORD macro in the scene shaders).
    */
    struct VkmVertexLayout
    {
        static constexpr uint32_t MAX_ATTRIBUTES = static_cast<uint32_t>(VkmVertexSemantic::Count);

        VkmVertexAttribute _attributes[MAX_ATTRIBUTES]{};
        uint32_t _attributeCount = 0;
        uint32_t _stride = 0;
        VkmVertexLayoutPreset _preset = VkmVertexLayoutPreset::StandardPBR;
    };

    const VkmVertexLayout& vkmGetVertexLayoutPreset(VkmVertexLayoutPreset preset);

    /*
    * @brief PSO option key of this preset's shader permutation, e.g. "standard_pbr" for
    * "model_viewer_pso[standard_pbr]". Must match the option names in the PSO JSON exactly --
    * TestVertexLayout asserts the spelling so a rename cannot silently stop resolving.
    */
    const char* vkmVertexLayoutPresetName(VkmVertexLayoutPreset preset);

    // nullptr when `layout` does not carry `semantic`.
    const VkmVertexAttribute* vkmFindVertexAttribute(const VkmVertexLayout& layout, VkmVertexSemantic semantic);

    /*
    * @brief The single conversion site between float components and a layout's storage format.
    *
    * The glTF importer, normal generation, bounds computation and the unit tests all go through
    * these, so support for a new VkmVertexAttributeFormat only ever has to be added in one place.
    *
    * Reading a semantic the layout does not carry zero-fills `out`; writing one is a no-op. That
    * is what lets the importer run the same code for every preset. Components beyond the
    * attribute's own component count are zero-filled on read and ignored on write.
    */
    void vkmReadVertexAttribute(const uint8_t* vertex,
                                const VkmVertexLayout& layout,
                                VkmVertexSemantic semantic,
                                float* out,
                                uint32_t componentCount);
    void vkmWriteVertexAttribute(uint8_t* vertex,
                                 const VkmVertexLayout& layout,
                                 VkmVertexSemantic semantic,
                                 const float* in,
                                 uint32_t componentCount);
} // namespace vkm
