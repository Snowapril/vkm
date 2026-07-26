#include <doctest/doctest.h>

#include <vkm/renderer/scene/vertex_layout.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using vkm::VkmVertexAttribute;
using vkm::VkmVertexAttributeFormat;
using vkm::VkmVertexLayout;
using vkm::VkmVertexLayoutPreset;
using vkm::VkmVertexSemantic;

namespace
{
const VkmVertexAttribute* requireAttribute(const VkmVertexLayout& layout, VkmVertexSemantic semantic)
{
    const VkmVertexAttribute* attribute = vkm::vkmFindVertexAttribute(layout, semantic);
    REQUIRE(attribute != nullptr);
    return attribute;
}
} // namespace

/*
* The shader-ABI guard: StandardPBR must stay byte-identical to the fixed 64-byte vertex the
* engine used before layouts existed (position@0, normal@16, uv0@32, tangent@48). The padding is
* part of the std430-like layout DXC emits for storage-buffer structs, so the shader-side
* VertexData fetch offsets depend on these exact numbers.
*/
TEST_CASE("VkmVertexLayout - StandardPBR reproduces the legacy 64-byte vertex layout") {
    const VkmVertexLayout& layout = vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::StandardPBR);

    CHECK(layout._stride == 64);
    CHECK(layout._attributeCount == 4);
    CHECK(layout._preset == VkmVertexLayoutPreset::StandardPBR);

    const VkmVertexAttribute* position = requireAttribute(layout, VkmVertexSemantic::Position);
    CHECK(position->_offset == 0);
    CHECK(position->_format == VkmVertexAttributeFormat::Float32x3);

    const VkmVertexAttribute* normal = requireAttribute(layout, VkmVertexSemantic::Normal);
    CHECK(normal->_offset == 16);
    CHECK(normal->_format == VkmVertexAttributeFormat::Float32x3);

    const VkmVertexAttribute* uv0 = requireAttribute(layout, VkmVertexSemantic::UV0);
    CHECK(uv0->_offset == 32);
    CHECK(uv0->_format == VkmVertexAttributeFormat::Float32x2);

    const VkmVertexAttribute* tangent = requireAttribute(layout, VkmVertexSemantic::Tangent);
    CHECK(tangent->_offset == 48);
    CHECK(tangent->_format == VkmVertexAttributeFormat::Float32x4);
}

TEST_CASE("VkmVertexLayout - every preset has a word-addressable stride") {
    for (uint8_t i = 0; i < static_cast<uint8_t>(VkmVertexLayoutPreset::Count); ++i)
    {
        const VkmVertexLayoutPreset preset = static_cast<VkmVertexLayoutPreset>(i);
        const VkmVertexLayout& layout = vkm::vkmGetVertexLayoutPreset(preset);

        CHECK(layout._preset == preset);
        CHECK(layout._attributeCount > 0);
        CHECK(layout._attributeCount <= VkmVertexLayout::MAX_ATTRIBUTES);
        // Geometry pools are indexed in u32 words on every backend.
        CHECK(layout._stride % 4 == 0);

        // Every attribute must fit inside the stride, and Position is mandatory.
        CHECK(vkm::vkmFindVertexAttribute(layout, VkmVertexSemantic::Position) != nullptr);
        for (uint32_t a = 0; a < layout._attributeCount; ++a)
        {
            const VkmVertexAttribute& attribute = layout._attributes[a];
            CHECK(attribute._offset + vkm::vkmGetVertexAttributeFormatSize(attribute._format) <= layout._stride);
        }
    }
}

TEST_CASE("VkmVertexLayout - PositionOnly and Compact strides") {
    CHECK(vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::PositionOnly)._stride == 16);
    CHECK(vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::PositionOnly)._attributeCount == 1);

    const VkmVertexLayout& compact = vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::Compact);
    CHECK(compact._stride == 32);
    CHECK(compact._attributeCount == 4);
    CHECK(requireAttribute(compact, VkmVertexSemantic::Position)->_format == VkmVertexAttributeFormat::Float32x3);
    CHECK(requireAttribute(compact, VkmVertexSemantic::Normal)->_format == VkmVertexAttributeFormat::Snorm8x4);
    CHECK(requireAttribute(compact, VkmVertexSemantic::UV0)->_format == VkmVertexAttributeFormat::Float16x2);
    CHECK(requireAttribute(compact, VkmVertexSemantic::Tangent)->_format == VkmVertexAttributeFormat::Snorm8x4);
}

/*
* The preset names are the PSO JSON "options" keys: the draw path resolves
* "model_viewer_pso[<name>]" at runtime, so a rename here silently stops resolving the
* permutation instead of failing to build.
*/
TEST_CASE("VkmVertexLayout - preset names match the PSO option keys") {
    CHECK(std::string(vkm::vkmVertexLayoutPresetName(VkmVertexLayoutPreset::PositionOnly)) == "position_only");
    CHECK(std::string(vkm::vkmVertexLayoutPresetName(VkmVertexLayoutPreset::StandardPBR)) == "standard_pbr");
    CHECK(std::string(vkm::vkmVertexLayoutPresetName(VkmVertexLayoutPreset::Compact)) == "compact");
}

TEST_CASE("vkmReadVertexAttribute - zero-fills a semantic the layout does not carry") {
    const VkmVertexLayout& layout = vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::PositionOnly);
    CHECK(vkm::vkmFindVertexAttribute(layout, VkmVertexSemantic::Normal) == nullptr);

    // Non-zero backing bytes so a missing zero-fill would show up as garbage rather than 0.
    std::vector<uint8_t> vertex(layout._stride, 0xAB);
    float normal[3] = { 1.0f, 2.0f, 3.0f };
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Normal, normal, 3);
    CHECK(normal[0] == 0.0f);
    CHECK(normal[1] == 0.0f);
    CHECK(normal[2] == 0.0f);
}

TEST_CASE("vkmWriteVertexAttribute - is a no-op for a semantic the layout does not carry") {
    const VkmVertexLayout& layout = vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::PositionOnly);

    std::vector<uint8_t> vertex(layout._stride, 0x00);
    const std::vector<uint8_t> before = vertex;
    const float uv[2] = { 0.25f, 0.75f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::UV0, uv, 2);
    CHECK(std::memcmp(vertex.data(), before.data(), vertex.size()) == 0);
}

TEST_CASE("vkmWrite/ReadVertexAttribute - exact round-trip for the float32 formats") {
    const VkmVertexLayout& layout = vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::StandardPBR);
    std::vector<uint8_t> vertex(layout._stride, 0x00);

    const float position[3] = { -1.5f, 0.0f, 2.25f };
    const float normal[3] = { 0.0f, 1.0f, 0.0f };
    const float uv[2] = { 0.125f, 0.875f };
    const float tangent[4] = { 1.0f, 0.0f, 0.0f, -1.0f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Position, position, 3);
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Normal, normal, 3);
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::UV0, uv, 2);
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Tangent, tangent, 4);

    float readPosition[3] = {};
    float readNormal[3] = {};
    float readUv[2] = {};
    float readTangent[4] = {};
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Position, readPosition, 3);
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Normal, readNormal, 3);
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::UV0, readUv, 2);
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Tangent, readTangent, 4);

    for (uint32_t i = 0; i < 3; ++i)
    {
        CHECK(readPosition[i] == position[i]);
        CHECK(readNormal[i] == normal[i]);
    }
    CHECK(readUv[0] == uv[0]);
    CHECK(readUv[1] == uv[1]);
    for (uint32_t i = 0; i < 4; ++i)
    {
        CHECK(readTangent[i] == tangent[i]);
    }
}

TEST_CASE("vkmWrite/ReadVertexAttribute - lossy round-trip for the packed Compact formats") {
    const VkmVertexLayout& layout = vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::Compact);
    std::vector<uint8_t> vertex(layout._stride, 0x00);

    // Position stays float32 even in Compact, so it must round-trip exactly.
    const float position[3] = { -1.5f, 0.0f, 2.25f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Position, position, 3);
    float readPosition[3] = {};
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Position, readPosition, 3);
    for (uint32_t i = 0; i < 3; ++i)
    {
        CHECK(readPosition[i] == position[i]);
    }

    // Snorm8x4: one step of the 127-level grid is ~0.008, so 0.01 is a safe tolerance.
    const float normal[4] = { 0.0f, -1.0f, 1.0f, 0.5f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Normal, normal, 4);
    float readNormal[4] = {};
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Normal, readNormal, 4);
    for (uint32_t i = 0; i < 4; ++i)
    {
        CHECK(readNormal[i] == doctest::Approx(normal[i]).epsilon(0.01));
    }

    // Float16x2 holds ~3 decimal digits; 1e-3 is comfortably outside its error for these values.
    const float uv[2] = { 0.125f, 0.875f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::UV0, uv, 2);
    float readUv[2] = {};
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::UV0, readUv, 2);
    CHECK(readUv[0] == doctest::Approx(uv[0]).epsilon(0.001));
    CHECK(readUv[1] == doctest::Approx(uv[1]).epsilon(0.001));
}

/*
* Asking for fewer components than the attribute stores must not decode a partial packed value,
* and asking for more must zero-fill the tail rather than read past the attribute.
*/
TEST_CASE("vkmRead/WriteVertexAttribute - component count mismatches are handled") {
    const VkmVertexLayout& layout = vkm::vkmGetVertexLayoutPreset(VkmVertexLayoutPreset::StandardPBR);
    std::vector<uint8_t> vertex(layout._stride, 0x00);

    // Write 4 tangent components, read back only 3.
    const float tangent[4] = { 0.5f, -0.5f, 0.25f, -1.0f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Tangent, tangent, 4);
    float readThree[3] = {};
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Tangent, readThree, 3);
    CHECK(readThree[0] == tangent[0]);
    CHECK(readThree[1] == tangent[1]);
    CHECK(readThree[2] == tangent[2]);

    // UV0 stores 2 components; asking for 4 zero-fills the tail.
    const float uv[2] = { 0.25f, 0.75f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::UV0, uv, 2);
    float readFour[4] = { 9.0f, 9.0f, 9.0f, 9.0f };
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::UV0, readFour, 4);
    CHECK(readFour[0] == uv[0]);
    CHECK(readFour[1] == uv[1]);
    CHECK(readFour[2] == 0.0f);
    CHECK(readFour[3] == 0.0f);

    // Writing fewer components than stored zero-fills the rest rather than leaving old bytes.
    const float partialTangent[2] = { 1.0f, 1.0f };
    vkm::vkmWriteVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Tangent, partialTangent, 2);
    float readAfterPartial[4] = {};
    vkm::vkmReadVertexAttribute(vertex.data(), layout, VkmVertexSemantic::Tangent, readAfterPartial, 4);
    CHECK(readAfterPartial[0] == 1.0f);
    CHECK(readAfterPartial[1] == 1.0f);
    CHECK(readAfterPartial[2] == 0.0f);
    CHECK(readAfterPartial[3] == 0.0f);
}

TEST_CASE("vkmGetVertexAttributeFormatSize - sizes and component counts agree with the formats") {
    CHECK(vkm::vkmGetVertexAttributeFormatSize(VkmVertexAttributeFormat::Float32x2) == 8);
    CHECK(vkm::vkmGetVertexAttributeFormatSize(VkmVertexAttributeFormat::Float32x3) == 12);
    CHECK(vkm::vkmGetVertexAttributeFormatSize(VkmVertexAttributeFormat::Float32x4) == 16);
    CHECK(vkm::vkmGetVertexAttributeFormatSize(VkmVertexAttributeFormat::Float16x2) == 4);
    CHECK(vkm::vkmGetVertexAttributeFormatSize(VkmVertexAttributeFormat::Snorm8x4) == 4);

    CHECK(vkm::vkmGetVertexAttributeFormatComponentCount(VkmVertexAttributeFormat::Float32x2) == 2);
    CHECK(vkm::vkmGetVertexAttributeFormatComponentCount(VkmVertexAttributeFormat::Float32x3) == 3);
    CHECK(vkm::vkmGetVertexAttributeFormatComponentCount(VkmVertexAttributeFormat::Float32x4) == 4);
    CHECK(vkm::vkmGetVertexAttributeFormatComponentCount(VkmVertexAttributeFormat::Float16x2) == 2);
    CHECK(vkm::vkmGetVertexAttributeFormatComponentCount(VkmVertexAttributeFormat::Snorm8x4) == 4);
}
