#include <doctest/doctest.h>

#include <vkm/renderer/scene/scene_geometry_pool.h>

#include <cstdint>
#include <string>
#include <vector>

using vkm::VkmSceneGeometryPool;
using vkm::VkmSceneMesh;
using vkm::VkmVertexLayoutPreset;
using vkm::VkmVertexSemantic;

namespace
{
// A mesh of `vertexCount` vertices whose positions are (i, 0, 0), and one triangle per three
// vertices indexing them in order.
VkmSceneMesh makeMesh(VkmVertexLayoutPreset preset, uint32_t vertexCount, uint32_t indexCount)
{
    VkmSceneMesh mesh;
    mesh._layout = vkm::vkmGetVertexLayoutPreset(preset);
    mesh._vertexCount = vertexCount;
    mesh._vertexData.assign(static_cast<size_t>(vertexCount) * mesh._layout._stride, 0);

    for (uint32_t i = 0; i < vertexCount; ++i)
    {
        const float position[3] = { static_cast<float>(i), 0.0f, 0.0f };
        vkm::vkmWriteVertexAttribute(mesh._vertexData.data() + static_cast<size_t>(i) * mesh._layout._stride,
                                     mesh._layout, VkmVertexSemantic::Position, position, 3);
    }

    mesh._indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i)
    {
        mesh._indices[i] = i % vertexCount;
    }
    return mesh;
}
} // namespace

TEST_CASE("VkmSceneGeometryPool - appends meshes at increasing word and element offsets") {
    VkmSceneGeometryPool pool(VkmVertexLayoutPreset::StandardPBR);
    CHECK(pool.isEmpty());
    CHECK(pool.getLayout()._preset == VkmVertexLayoutPreset::StandardPBR);
    CHECK(pool.getVertexPoolSlot() == vkm::INVALID_VALUE32);
    CHECK(pool.getIndexPoolSlot() == vkm::INVALID_VALUE32);

    std::string error;
    VkmSceneGeometryPool::MeshRange first{};
    REQUIRE(pool.appendMesh(makeMesh(VkmVertexLayoutPreset::StandardPBR, 3, 3), &first, &error));
    CHECK(error.empty());
    CHECK(first._vertexWordOffset == 0);
    CHECK(first._vertexCount == 3);
    CHECK(first._indexOffset == 0);
    CHECK(first._indexCount == 3);
    CHECK_FALSE(pool.isEmpty());

    VkmSceneGeometryPool::MeshRange second{};
    REQUIRE(pool.appendMesh(makeMesh(VkmVertexLayoutPreset::StandardPBR, 4, 6), &second, &error));
    // The first mesh occupied 3 * 64 bytes == 48 u32 words.
    CHECK(second._vertexWordOffset == 48);
    CHECK(second._vertexCount == 4);
    CHECK(second._indexOffset == 3);
    CHECK(second._indexCount == 6);
}

/*
* Indices deliberately stay mesh-local: MeshRange::_vertexWordOffset is what supplies the base
* in-shader (see VkmObjectData::_vertexWordOffset), so rebasing on the CPU would add it twice.
*/
TEST_CASE("VkmSceneGeometryPool - appended indices are not rebased onto the pool") {
    VkmSceneGeometryPool pool(VkmVertexLayoutPreset::StandardPBR);
    std::string error;

    VkmSceneGeometryPool::MeshRange first{};
    REQUIRE(pool.appendMesh(makeMesh(VkmVertexLayoutPreset::StandardPBR, 3, 3), &first, &error));

    VkmSceneMesh secondMesh = makeMesh(VkmVertexLayoutPreset::StandardPBR, 3, 3);
    secondMesh._indices = { 2, 1, 0 };
    VkmSceneGeometryPool::MeshRange second{};
    REQUIRE(pool.appendMesh(secondMesh, &second, &error));

    // The second mesh's index values must still address its own vertices, 0-based.
    CHECK(second._indexOffset == 3);
    CHECK(second._indexCount == 3);
    CHECK(second._vertexWordOffset == 48);
}

TEST_CASE("VkmSceneGeometryPool - a word offset stays addressable as the pool grows") {
    VkmSceneGeometryPool pool(VkmVertexLayoutPreset::PositionOnly);
    std::string error;

    // PositionOnly is 16 bytes == 4 words per vertex.
    VkmSceneGeometryPool::MeshRange range{};
    REQUIRE(pool.appendMesh(makeMesh(VkmVertexLayoutPreset::PositionOnly, 100, 99), &range, &error));
    CHECK(range._vertexWordOffset == 0);

    REQUIRE(pool.appendMesh(makeMesh(VkmVertexLayoutPreset::PositionOnly, 5, 3), &range, &error));
    CHECK(range._vertexWordOffset == 400);
    CHECK(range._indexOffset == 99);
}

TEST_CASE("VkmSceneGeometryPool - reports its layout's stride through getLayout") {
    CHECK(VkmSceneGeometryPool(VkmVertexLayoutPreset::PositionOnly).getLayout()._stride == 16);
    CHECK(VkmSceneGeometryPool(VkmVertexLayoutPreset::StandardPBR).getLayout()._stride == 64);
    CHECK(VkmSceneGeometryPool(VkmVertexLayoutPreset::Compact).getLayout()._stride == 32);
}
