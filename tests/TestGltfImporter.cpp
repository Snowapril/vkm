#include <doctest/doctest.h>

#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene_model.h>

#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <string>
#include <vector>

namespace
{
// Both fixtures describe the same CCW triangle in the XY plane:
//   p0 (0,0,0)  p1 (1,0,0)  p2 (0,1,0)
const std::string kGltfPath = std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf";
const std::string kGlbPath = std::string(RESOURCES_DIR) + "tests/gltf_triangle.glb";
const std::string kTexturedPath = std::string(RESOURCES_DIR) + "tests/gltf_textured.gltf";

// Keeps the file's own vertex/index ordering so the assertions below can address
// individual vertices.
vkm::VkmGltfImportOptions unoptimizedOptions()
{
    vkm::VkmGltfImportOptions options;
    options._optimizeMeshes = false;
    return options;
}

vkm::VkmGltfImportOptions unoptimizedOptions(vkm::VkmVertexLayoutPreset preset)
{
    vkm::VkmGltfImportOptions options = unoptimizedOptions();
    options._vertexLayout = preset;
    return options;
}

std::vector<char> readFileBytes(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Vertices are interleaved bytes in the mesh's own layout, so tests read them the same way the
// engine does rather than reaching into a fixed struct.
std::vector<float> readAttribute(const vkm::VkmSceneMesh& mesh,
                                 uint32_t vertexIndex,
                                 vkm::VkmVertexSemantic semantic,
                                 uint32_t componentCount)
{
    std::vector<float> out(componentCount, 0.0f);
    vkm::vkmReadVertexAttribute(mesh._vertexData.data() + static_cast<size_t>(vertexIndex) * mesh._layout._stride,
                                mesh._layout, semantic, out.data(), componentCount);
    return out;
}
} // namespace

TEST_CASE("importGltfModel - imports meshes, materials and the node hierarchy of a .gltf") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE(vkm::importGltfModel(kGltfPath, &model, &error, unoptimizedOptions()));
    CHECK(error.empty());

    REQUIRE(model._meshes.size() == 1);
    const vkm::VkmSceneMesh& mesh = model._meshes[0];
    CHECK(mesh._vertexCount == 3);
    CHECK(mesh._indices.size() == 3);
    CHECK(mesh._materialIndex == 0);
    CHECK(mesh._layout._preset == vkm::VkmVertexLayoutPreset::StandardPBR);
    CHECK(mesh._vertexData.size() == 3 * mesh._layout._stride);

    CHECK(readAttribute(mesh, 1, vkm::VkmVertexSemantic::Position, 3)[0] == doctest::Approx(1.0f));
    CHECK(readAttribute(mesh, 2, vkm::VkmVertexSemantic::Position, 3)[1] == doctest::Approx(1.0f));
    CHECK(readAttribute(mesh, 0, vkm::VkmVertexSemantic::Normal, 3)[2] == doctest::Approx(1.0f));
    CHECK(readAttribute(mesh, 1, vkm::VkmVertexSemantic::UV0, 2)[0] == doctest::Approx(1.0f));

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
    REQUIRE(mesh._vertexCount == 3);
    CHECK(mesh._materialIndex == vkm::INVALID_VALUE32);

    // A CCW triangle in the XY plane must produce a unit +Z normal on every vertex.
    for (uint32_t i = 0; i < mesh._vertexCount; ++i)
    {
        const std::vector<float> normal = readAttribute(mesh, i, vkm::VkmVertexSemantic::Normal, 3);
        CHECK(normal[0] == doctest::Approx(0.0f));
        CHECK(normal[1] == doctest::Approx(0.0f));
        CHECK(normal[2] == doctest::Approx(1.0f));
    }
}

TEST_CASE("importGltfModel - packs into the requested vertex layout preset") {
    SUBCASE("PositionOnly drops the shading attributes and skips normal generation") {
        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE(vkm::importGltfModel(kGlbPath, &model, &error,
                                     unoptimizedOptions(vkm::VkmVertexLayoutPreset::PositionOnly)));

        REQUIRE(model._meshes.size() == 1);
        const vkm::VkmSceneMesh& mesh = model._meshes[0];
        CHECK(mesh._layout._preset == vkm::VkmVertexLayoutPreset::PositionOnly);
        CHECK(mesh._layout._stride == 16);
        CHECK(mesh._vertexCount == 3);
        CHECK(mesh._vertexData.size() == 3 * 16);
        CHECK(vkm::vkmFindVertexAttribute(mesh._layout, vkm::VkmVertexSemantic::Normal) == nullptr);

        // Positions survive; the .glb has no normals, and this layout has nowhere to put the
        // generated ones, so generateNormals must be a no-op rather than a buffer overrun.
        CHECK(readAttribute(mesh, 1, vkm::VkmVertexSemantic::Position, 3)[0] == doctest::Approx(1.0f));
        CHECK(readAttribute(mesh, 2, vkm::VkmVertexSemantic::Position, 3)[1] == doctest::Approx(1.0f));
        // Bounds are computed from positions, so they are unaffected by the dropped attributes.
        CHECK(mesh._bounds._max.x == doctest::Approx(1.0f));
        CHECK(mesh._bounds._max.y == doctest::Approx(1.0f));
    }

    SUBCASE("Compact quantizes the normal and uv but keeps the position exact") {
        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE(vkm::importGltfModel(kGltfPath, &model, &error,
                                     unoptimizedOptions(vkm::VkmVertexLayoutPreset::Compact)));

        REQUIRE(model._meshes.size() == 1);
        const vkm::VkmSceneMesh& mesh = model._meshes[0];
        CHECK(mesh._layout._preset == vkm::VkmVertexLayoutPreset::Compact);
        CHECK(mesh._layout._stride == 32);
        CHECK(mesh._vertexCount == 3);
        CHECK(mesh._vertexData.size() == 3 * 32);

        CHECK(readAttribute(mesh, 1, vkm::VkmVertexSemantic::Position, 3)[0] == doctest::Approx(1.0f));
        // Snorm8x4 grid step is ~1/127, so 0.01 is comfortably outside the quantization error.
        CHECK(readAttribute(mesh, 0, vkm::VkmVertexSemantic::Normal, 3)[2] == doctest::Approx(1.0f).epsilon(0.01));
        // Float16x2 holds ~3 decimal digits.
        CHECK(readAttribute(mesh, 1, vkm::VkmVertexSemantic::UV0, 2)[0] == doctest::Approx(1.0f).epsilon(0.001));
    }
}

/*
* meshopt_optimizeVertexFetch reorders the vertex bytes using the layout's stride, so a
* non-StandardPBR preset going through the optimizer is the case most likely to mis-sort.
*/
TEST_CASE("importGltfModel - optimized import of a non-default layout keeps the geometry intact") {
    vkm::VkmGltfImportOptions options;
    options._vertexLayout = vkm::VkmVertexLayoutPreset::Compact;

    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE(vkm::importGltfModel(kGltfPath, &model, &error, options));

    REQUIRE(model._meshes.size() == 1);
    const vkm::VkmSceneMesh& mesh = model._meshes[0];
    CHECK(mesh._vertexCount == 3);
    CHECK(mesh._indices.size() == 3);
    CHECK(mesh._vertexData.size() == static_cast<size_t>(mesh._vertexCount) * mesh._layout._stride);

    // The triangle's three corners must still be present, whatever order the optimizer chose.
    bool sawUnitX = false;
    bool sawUnitY = false;
    for (uint32_t i = 0; i < mesh._vertexCount; ++i)
    {
        const std::vector<float> position = readAttribute(mesh, i, vkm::VkmVertexSemantic::Position, 3);
        sawUnitX = sawUnitX || position[0] == doctest::Approx(1.0f);
        sawUnitY = sawUnitY || position[1] == doctest::Approx(1.0f);
    }
    CHECK(sawUnitX);
    CHECK(sawUnitY);
}

TEST_CASE("importGltfModel - optimized import keeps the geometry intact") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE(vkm::importGltfModel(kGltfPath, &model, &error, vkm::VkmGltfImportOptions{}));

    REQUIRE(model._meshes.size() == 1);
    CHECK(model._meshes[0]._vertexCount == 3);
    CHECK(model._meshes[0]._indices.size() == 3);
}

/*
* Materials referencing textures is the point; what is easy to get wrong is everything around it.
* A URI is relative to the glTF document, so it only resolves alongside that file's directory, and
* it may be percent-encoded -- the red image's name is written with %5F here so a decoder that is
* missing shows up as a path that cannot be opened rather than as a subtly wrong one.
*/
TEST_CASE("importGltfModel - resolves material texture references to image paths") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE_MESSAGE(vkm::importGltfModel(kTexturedPath, &model, &error), error);

    REQUIRE(model._images.size() == 2);
    REQUIRE(model._materials.size() == 1);
    const vkm::VkmSceneMaterial& material = model._materials[0];

    SUBCASE("each channel points at the image the glTF named") {
        CHECK(material._baseColorImage == 0);
        CHECK(material._normalImage == 1);
        // Absent in the fixture: a material without a texture for a channel has to be
        // distinguishable from one pointing at image 0, or every such material samples image 0.
        CHECK(material._metallicRoughnessImage == vkm::INVALID_VALUE32);
        CHECK(material._emissiveImage == vkm::INVALID_VALUE32);
    }

    SUBCASE("factors still arrive alongside the textures") {
        // glTF multiplies factor by texture, so losing the factor when a texture appears would
        // silently change the material rather than fail.
        CHECK(material._baseColorFactor.r == doctest::Approx(0.25f));
        CHECK(material._roughnessFactor == doctest::Approx(0.75f));
    }

    SUBCASE("URIs resolve next to the glTF, percent-encoding decoded") {
        for (const vkm::VkmSceneImage& image : model._images) {
            REQUIRE_FALSE(image._uri.empty());
            // The real check: the path can actually be opened. A URI left relative, or left
            // percent-encoded, produces a plausible-looking string that no loader can read.
            std::error_code ec;
            CHECK_MESSAGE(std::filesystem::is_regular_file(image._uri, ec), image._uri);
        }
        CHECK(model._images[1]._uri.find("reference_red_64x64.png") != std::string::npos);
        CHECK(model._images[1]._uri.find('%') == std::string::npos);
    }
}

TEST_CASE("importGltfModel - a material with no textures reports none") {
    vkm::VkmSceneModel model;
    std::string error;
    REQUIRE_MESSAGE(vkm::importGltfModel(kGltfPath, &model, &error), error);
    REQUIRE(model._materials.size() == 1);
    CHECK(model._images.empty());
    CHECK(model._materials[0]._baseColorImage == vkm::INVALID_VALUE32);
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
