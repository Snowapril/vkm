#include <doctest/doctest.h>

#include <vkm/renderer/scene/light_table.h>
#include <vkm/renderer/scene/scene_model.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/vec3.hpp>

#include <cstring>
#include <vector>

// The light-table builder is free-standing and driver-free (like vkmBuildNeighbourOffsets), so
// the CPU half of next-event estimation -- triangle gather, areas, the power CDF -- is testable
// without a GPU. A wrong CDF is a wrong sampling pdf, which shifts the estimator's MEAN; these
// are the cheapest place to catch that class.

namespace
{
    // A mesh in the StandardPBR layout holding `positions` as its vertices, one triangle per
    // consecutive index triple.
    vkm::VkmSceneMesh makeMesh(const std::vector<glm::vec3>& positions,
                               const std::vector<uint32_t>& indices)
    {
        vkm::VkmSceneMesh mesh;
        mesh._layout = vkm::vkmGetVertexLayoutPreset(vkm::VkmVertexLayoutPreset::StandardPBR);
        mesh._vertexCount = static_cast<uint32_t>(positions.size());
        mesh._vertexData.assign(static_cast<size_t>(mesh._vertexCount) * mesh._layout._stride, 0);
        for (size_t i = 0; i < positions.size(); ++i)
        {
            const float position[3] = { positions[i].x, positions[i].y, positions[i].z };
            vkm::vkmWriteVertexAttribute(mesh._vertexData.data() + i * mesh._layout._stride,
                                         mesh._layout, vkm::VkmVertexSemantic::Position, position, 3);
        }
        mesh._indices = indices;
        return mesh;
    }
}

TEST_CASE("vkmGatherEmissiveTriangles - extracts positions through the vertex layout")
{
    const vkm::VkmSceneMesh mesh = makeMesh(
        { { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }, { 0.0f, 3.0f, 0.0f }, { 5.0f, 5.0f, 5.0f } },
        { 0, 1, 2, 1, 3, 2 });

    std::vector<vkm::VkmLightTableTriangle> triangles;
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(1.0f, 2.0f, 3.0f), &triangles);

    REQUIRE(triangles.size() == 2);
    CHECK(triangles[0]._p0[0] == 0.0f);
    CHECK(triangles[0]._p1[0] == 2.0f);
    CHECK(triangles[0]._p2[1] == 3.0f);
    CHECK(triangles[1]._p1[2] == 5.0f);
    CHECK(triangles[0]._radiance[1] == 2.0f);
    // Appended, not overwritten: a second mesh's triangles join the first's.
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(1.0f), &triangles);
    CHECK(triangles.size() == 4);
}

TEST_CASE("vkmFinalizeLightTable - areas, monotonic CDF, exact terminal 1")
{
    // Two right triangles in z = 0: legs (2,3) -> area 3, legs (2,1) -> area 1. Equal radiance,
    // so power splits 3:1 and the first CDF entry is exactly 0.75.
    std::vector<vkm::VkmLightTableTriangle> triangles;
    const vkm::VkmSceneMesh mesh = makeMesh(
        { { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }, { 0.0f, 3.0f, 0.0f },
          { 10.0f, 0.0f, 0.0f }, { 12.0f, 0.0f, 0.0f }, { 10.0f, 1.0f, 0.0f } },
        { 0, 1, 2, 3, 4, 5 });
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(1.0f), &triangles);

    REQUIRE(vkm::vkmFinalizeLightTable(&triangles) == 2);
    CHECK(triangles[0]._area == doctest::Approx(3.0f));
    CHECK(triangles[1]._area == doctest::Approx(1.0f));
    CHECK(triangles[0]._cdf == doctest::Approx(0.75f));
    CHECK(triangles[1]._cdf == 1.0f); // exact, not approximate: the search's ceiling
    CHECK(triangles[0]._cdf < triangles[1]._cdf);
}

TEST_CASE("vkmFinalizeLightTable - power weights radiance as luminance, not per channel")
{
    // Same area, radiance (4,0,0) vs (0,1,0): power ratio = 4*0.2126 : 1*0.7152 = 0.8504:0.7152.
    std::vector<vkm::VkmLightTableTriangle> triangles;
    const vkm::VkmSceneMesh mesh = makeMesh(
        { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } }, { 0, 1, 2 });
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(4.0f, 0.0f, 0.0f), &triangles);
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(0.0f, 1.0f, 0.0f), &triangles);

    REQUIRE(vkm::vkmFinalizeLightTable(&triangles) == 2);
    CHECK(triangles[0]._cdf == doctest::Approx(0.8504f / (0.8504f + 0.7152f)));
}

TEST_CASE("vkmFinalizeLightTable - drops degenerate and black entries, keeps the CDF strict")
{
    std::vector<vkm::VkmLightTableTriangle> triangles;
    const vkm::VkmSceneMesh mesh = makeMesh(
        { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
          { 4.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, { 6.0f, 0.0f, 0.0f } }, // collinear
        { 0, 1, 2, 3, 4, 5 });
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(1.0f), &triangles);          // 1 real, 1 degenerate
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(0.0f), &triangles);          // both black

    REQUIRE(vkm::vkmFinalizeLightTable(&triangles) == 1);
    CHECK(triangles.size() == 1);
    CHECK(triangles[0]._cdf == 1.0f);
}

TEST_CASE("vkmFinalizeLightTable - an empty or all-black table finalizes to zero entries")
{
    std::vector<vkm::VkmLightTableTriangle> empty;
    CHECK(vkm::vkmFinalizeLightTable(&empty) == 0);

    std::vector<vkm::VkmLightTableTriangle> black;
    const vkm::VkmSceneMesh mesh = makeMesh(
        { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } }, { 0, 1, 2 });
    vkm::vkmGatherEmissiveTriangles(mesh, glm::vec3(0.0f), &black);
    CHECK(vkm::vkmFinalizeLightTable(&black) == 0);
    CHECK(black.empty());
}
