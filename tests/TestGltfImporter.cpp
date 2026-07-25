#include <doctest/doctest.h>

#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene_model.h>

#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{
// Both fixtures describe the same CCW triangle in the XY plane:
//   p0 (0,0,0)  p1 (1,0,0)  p2 (0,1,0)
const std::string kGltfPath = std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf";
const std::string kGlbPath = std::string(RESOURCES_DIR) + "tests/gltf_triangle.glb";

// Keeps the file's own vertex/index ordering so the assertions below can address
// individual vertices.
vkm::VkmGltfImportOptions unoptimizedOptions()
{
    vkm::VkmGltfImportOptions options;
    options._optimizeMeshes = false;
    return options;
}

std::vector<char> readFileBytes(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("importGltfModel - imports meshes, materials and the node hierarchy of a .gltf") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE(vkm::importGltfModel(kGltfPath, &model, &error, unoptimizedOptions()));
    CHECK(error.empty());

    REQUIRE(model._meshes.size() == 1);
    const vkm::VkmSceneMesh& mesh = model._meshes[0];
    CHECK(mesh._vertices.size() == 3);
    CHECK(mesh._indices.size() == 3);
    CHECK(mesh._materialIndex == 0);

    CHECK(mesh._vertices[1]._position[0] == doctest::Approx(1.0f));
    CHECK(mesh._vertices[2]._position[1] == doctest::Approx(1.0f));
    CHECK(mesh._vertices[0]._normal[2] == doctest::Approx(1.0f));
    CHECK(mesh._vertices[1]._uv0[0] == doctest::Approx(1.0f));

    CHECK(mesh._bounds._valid);
    CHECK(mesh._bounds._min.x == doctest::Approx(0.0f));
    CHECK(mesh._bounds._max.x == doctest::Approx(1.0f));
    CHECK(mesh._bounds._max.y == doctest::Approx(1.0f));

    REQUIRE(model._materials.size() == 1);
    const vkm::VkmSceneMaterial& material = model._materials[0];
    CHECK(material._name == "TestMaterial");
    CHECK(material._baseColorFactor.r == doctest::Approx(0.25f));
    CHECK(material._baseColorFactor.b == doctest::Approx(0.75f));
    CHECK(material._metallicFactor == doctest::Approx(0.25f));
    CHECK(material._roughnessFactor == doctest::Approx(0.75f));
    CHECK(material._emissiveFactor.g == doctest::Approx(0.2f));

    REQUIRE(model._nodes.size() == 2);
    REQUIRE(model._rootNodeIndices.size() == 1);
    CHECK(model._rootNodeIndices[0] == 0);
    CHECK(model._nodes[0]._name == "root");
    CHECK(model._nodes[0]._childIndices == std::vector<uint32_t>{1});
    CHECK(model._nodes[0]._meshIndices.empty());
    CHECK(model._nodes[1]._meshIndices == std::vector<uint32_t>{0});

    // The root's translation must survive as the matrix' fourth column.
    CHECK(model._nodes[0]._localTransform[3][0] == doctest::Approx(1.0f));
    CHECK(model._nodes[0]._localTransform[3][1] == doctest::Approx(2.0f));
    CHECK(model._nodes[0]._localTransform[3][2] == doctest::Approx(3.0f));
}

TEST_CASE("VkmSceneModel - flattens the hierarchy into world-space draw items") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE(vkm::importGltfModel(kGltfPath, &model, &error, unoptimizedOptions()));

    const std::vector<vkm::VkmSceneModel::DrawItem> drawList = model.buildDrawList();
    REQUIRE(drawList.size() == 1);
    CHECK(drawList[0]._meshIndex == 0);
    CHECK(drawList[0]._nodeIndex == 1);
    // The mesh sits on the child, so it inherits the root's translation.
    CHECK(drawList[0]._worldTransform[3][0] == doctest::Approx(1.0f));
    CHECK(drawList[0]._worldTransform[3][1] == doctest::Approx(2.0f));
    CHECK(drawList[0]._worldTransform[3][2] == doctest::Approx(3.0f));

    const vkm::VkmSceneAABB worldBounds = model.computeWorldBounds();
    REQUIRE(worldBounds._valid);
    CHECK(worldBounds._min.x == doctest::Approx(1.0f));
    CHECK(worldBounds._max.x == doctest::Approx(2.0f));
    CHECK(worldBounds._max.y == doctest::Approx(3.0f));
    CHECK(worldBounds._max.z == doctest::Approx(3.0f));

    CHECK(model.getTotalVertexCount() == 3);
    CHECK(model.getTotalIndexCount() == 3);
}

TEST_CASE("importGltfModel - reads a binary .glb and generates the missing normals") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE(vkm::importGltfModel(kGlbPath, &model, &error, unoptimizedOptions()));

    REQUIRE(model._meshes.size() == 1);
    const vkm::VkmSceneMesh& mesh = model._meshes[0];
    REQUIRE(mesh._vertices.size() == 3);
    CHECK(mesh._materialIndex == vkm::INVALID_VALUE32);

    // A CCW triangle in the XY plane must produce a unit +Z normal on every vertex.
    for (const vkm::VkmSceneVertex& vertex : mesh._vertices)
    {
        CHECK(vertex._normal[0] == doctest::Approx(0.0f));
        CHECK(vertex._normal[1] == doctest::Approx(0.0f));
        CHECK(vertex._normal[2] == doctest::Approx(1.0f));
    }
}

TEST_CASE("importGltfModel - optimized import keeps the geometry intact") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE(vkm::importGltfModel(kGltfPath, &model, &error, vkm::VkmGltfImportOptions{}));

    REQUIRE(model._meshes.size() == 1);
    CHECK(model._meshes[0]._vertices.size() == 3);
    CHECK(model._meshes[0]._indices.size() == 3);
}

TEST_CASE("importGltfModel - fails gracefully on a missing file") {
    vkm::VkmSceneModel model;
    std::string error;
    CHECK_FALSE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/does_not_exist.gltf", &model, &error));
    CHECK_FALSE(error.empty());
    CHECK(model._meshes.empty());
}

TEST_CASE("importGltfModel - fails gracefully on a truncated .glb") {
    const std::vector<char> bytes = readFileBytes(kGlbPath);
    REQUIRE(bytes.size() > 64);

    const std::string truncatedPath = std::string(RESOURCES_DIR) + "tests/gltf_triangle_truncated.glb";
    {
        std::ofstream truncated(truncatedPath, std::ios::binary);
        truncated.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
    }

    vkm::VkmSceneModel model;
    std::string error;
    CHECK_FALSE(vkm::importGltfModel(truncatedPath, &model, &error));
    CHECK_FALSE(error.empty());

    std::remove(truncatedPath.c_str());
}
