#include <doctest/doctest.h>

#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/scene_model.h>

#include <glm/ext/matrix_transform.hpp>

#include <cstdint>
#include <string>
#include <vector>

using vkm::VkmScene;
using vkm::VkmSceneMesh;
using vkm::VkmSceneModel;
using vkm::VkmVertexLayoutPreset;

namespace
{
// One triangle in `preset`'s layout, referencing material `materialIndex`.
VkmSceneMesh makeTriangleMesh(VkmVertexLayoutPreset preset, uint32_t materialIndex)
{
    VkmSceneMesh mesh;
    mesh._layout = vkm::vkmGetVertexLayoutPreset(preset);
    mesh._vertexCount = 3;
    mesh._vertexData.assign(static_cast<size_t>(3) * mesh._layout._stride, 0);
    mesh._indices = { 0, 1, 2 };
    mesh._materialIndex = materialIndex;
    mesh._bounds.expand(glm::vec3(0.0f));
    mesh._bounds.expand(glm::vec3(1.0f, 1.0f, 0.0f));
    return mesh;
}

/*
* A model with one node per mesh, each node translated along X so the objects are distinguishable.
* Materials are one per mesh so material ordering is observable.
*/
VkmSceneModel makeModel(const std::vector<VkmVertexLayoutPreset>& presets,
                        const std::vector<uint32_t>& materialIndices)
{
    REQUIRE(presets.size() == materialIndices.size());

    VkmSceneModel model;
    for (size_t i = 0; i < presets.size(); ++i)
    {
        model._meshes.push_back(makeTriangleMesh(presets[i], materialIndices[i]));

        vkm::VkmSceneNode node;
        node._name = "node" + std::to_string(i);
        node._localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(i), 0.0f, 0.0f));
        node._meshIndices.push_back(static_cast<uint32_t>(i));
        model._nodes.push_back(node);
        model._rootNodeIndices.push_back(static_cast<uint32_t>(i));
    }

    // One material per distinct index referenced above.
    uint32_t materialCount = 0;
    for (uint32_t index : materialIndices)
    {
        materialCount = std::max(materialCount, index + 1);
    }
    model._materials.resize(materialCount);
    return model;
}
} // namespace

TEST_CASE("VkmScene - one batch per (pipeline, vertex layout, material) run") {
    VkmScene scene;
    std::string error;

    SUBCASE("one layout and one material produce one batch covering every object") {
        REQUIRE(scene.addModel(makeModel({ VkmVertexLayoutPreset::StandardPBR,
                                           VkmVertexLayoutPreset::StandardPBR,
                                           VkmVertexLayoutPreset::StandardPBR },
                                         { 0, 0, 0 }),
                               &error));
        CHECK(error.empty());
        REQUIRE(scene.getObjects().size() == 3);
        REQUIRE(scene.getDrawBatches().size() == 1);

        const VkmScene::DrawBatch& batch = scene.getDrawBatches()[0];
        CHECK(batch._layout == VkmVertexLayoutPreset::StandardPBR);
        CHECK(batch._pipelineId == 0);
        CHECK(batch._materialIndex == 0);
        CHECK(batch._firstObject == 0);
        CHECK(batch._objectCount == 3);
    }

    SUBCASE("mixed layouts split into one batch each, contiguous and covering") {
        // One material throughout, so the split here is the layout's doing alone.
        REQUIRE(scene.addModel(makeModel({ VkmVertexLayoutPreset::StandardPBR,
                                           VkmVertexLayoutPreset::PositionOnly,
                                           VkmVertexLayoutPreset::StandardPBR,
                                           VkmVertexLayoutPreset::Compact },
                                         { 0, 0, 0, 0 }),
                               &error));
        REQUIRE(scene.getObjects().size() == 4);
        REQUIRE(scene.getDrawBatches().size() == 3);

        // Sorted by layout, so the batches come out in VkmVertexLayoutPreset order.
        CHECK(scene.getDrawBatches()[0]._layout == VkmVertexLayoutPreset::PositionOnly);
        CHECK(scene.getDrawBatches()[1]._layout == VkmVertexLayoutPreset::StandardPBR);
        CHECK(scene.getDrawBatches()[2]._layout == VkmVertexLayoutPreset::Compact);

        // Batches must tile the object array exactly: contiguous, in order, no gaps or overlaps.
        uint32_t expectedFirst = 0;
        for (const VkmScene::DrawBatch& batch : scene.getDrawBatches())
        {
            CHECK(batch._firstObject == expectedFirst);
            CHECK(batch._objectCount > 0);
            expectedFirst += batch._objectCount;
        }
        CHECK(expectedFirst == scene.getObjects().size());

        CHECK(scene.getDrawBatches()[1]._objectCount == 2); // the two StandardPBR meshes
    }
}

/*
* Material splits a batch, even though it is not pipeline state: a backend without bindless
* textures binds a per-material set-3 table before the draw, and a table is per-draw, so one
* material per batch is what makes that expressible. Objects are sorted by material first, so each
* material's run is contiguous and becomes exactly one batch.
*/
TEST_CASE("VkmScene - material index splits a batch and its runs stay contiguous") {
    VkmScene scene;
    std::string error;
    REQUIRE(scene.addModel(makeModel({ VkmVertexLayoutPreset::StandardPBR,
                                       VkmVertexLayoutPreset::StandardPBR,
                                       VkmVertexLayoutPreset::StandardPBR,
                                       VkmVertexLayoutPreset::StandardPBR },
                                     { 2, 0, 2, 1 }),
                           &error));

    // Three distinct materials (0, 1, 2) over four objects, so three batches -- one of them two
    // objects wide, which is what shows a run is merged rather than one batch per object.
    REQUIRE(scene.getDrawBatches().size() == 3);

    uint32_t expectedFirst = 0;
    uint32_t totalObjects = 0;
    uint32_t previousMaterial = 0;
    for (size_t i = 0; i < scene.getDrawBatches().size(); ++i)
    {
        const VkmScene::DrawBatch& batch = scene.getDrawBatches()[i];
        CHECK(batch._firstObject == expectedFirst);
        CHECK(batch._objectCount > 0);
        if (i > 0)
        {
            // Ascending, so no material is ever split across two non-adjacent batches.
            CHECK(batch._materialIndex > previousMaterial);
        }
        previousMaterial = batch._materialIndex;
        expectedFirst += batch._objectCount;
        totalObjects += batch._objectCount;
    }
    CHECK(totalObjects == 4);
    CHECK(scene.getDrawBatches()[2]._objectCount == 2); // the two objects sharing material 2
}

TEST_CASE("VkmScene - addModel is additive across calls") {
    VkmScene scene;
    std::string error;

    REQUIRE(scene.addModel(makeModel({ VkmVertexLayoutPreset::StandardPBR }, { 0 }), &error));
    CHECK(scene.getObjects().size() == 1);
    CHECK(scene.getDrawBatches().size() == 1);

    // A second model's materials are appended, so this object has material 1, not 0 -- it would
    // be its own batch on the material alone even without the differing layout.
    REQUIRE(scene.addModel(makeModel({ VkmVertexLayoutPreset::PositionOnly }, { 0 }), &error));
    CHECK(scene.getObjects().size() == 2);
    CHECK(scene.getDrawBatches().size() == 2);

    // The second model's geometry went into its own pool, so both batches hold one object.
    CHECK(scene.getDrawBatches()[0]._objectCount == 1);
    CHECK(scene.getDrawBatches()[1]._objectCount == 1);
}

TEST_CASE("VkmScene - meshes without geometry are dropped rather than drawn empty") {
    VkmSceneModel model = makeModel({ VkmVertexLayoutPreset::StandardPBR,
                                      VkmVertexLayoutPreset::StandardPBR },
                                    { 0, 0 });
    // Strip the second mesh's geometry, the way an unsupported glTF primitive would.
    model._meshes[1]._vertexData.clear();
    model._meshes[1]._vertexCount = 0;
    model._meshes[1]._indices.clear();

    VkmScene scene;
    std::string error;
    REQUIRE(scene.addModel(model, &error));

    CHECK(scene.getObjects().size() == 1);
    REQUIRE(scene.getDrawBatches().size() == 1);
    CHECK(scene.getDrawBatches()[0]._objectCount == 1);
}

TEST_CASE("VkmScene - world bounds follow the node transforms") {
    VkmScene scene;
    std::string error;
    REQUIRE(scene.addModel(makeModel({ VkmVertexLayoutPreset::StandardPBR,
                                       VkmVertexLayoutPreset::StandardPBR },
                                     { 0, 0 }),
                           &error));

    // Each mesh spans [0,1] in x and is translated by its node index, so the second reaches x = 2.
    const vkm::VkmSceneAABB bounds = scene.computeWorldBounds();
    REQUIRE(bounds._valid);
    CHECK(bounds._min.x == doctest::Approx(0.0f));
    CHECK(bounds._max.x == doctest::Approx(2.0f));
}

TEST_CASE("VkmScene - a batch's bounds enclose its objects in world space") {
    VkmScene scene;
    std::string error;
    REQUIRE(scene.addModel(makeModel({ VkmVertexLayoutPreset::StandardPBR,
                                       VkmVertexLayoutPreset::StandardPBR },
                                     { 0, 0 }),
                           &error));
    REQUIRE(scene.getDrawBatches().size() == 1);

    // A viewpoint culls a whole batch against this sphere, so it has to be conservative: every
    // corner of the geometry it covers must be inside it. Bounds left in object space -- the
    // meshes are identical and only their node transforms differ -- would enclose one triangle
    // and cull the batch away from viewpoints looking straight at the other.
    const vkm::VkmSceneAABB world = scene.computeWorldBounds();
    REQUIRE(world._valid);
    const VkmScene::DrawBatch& batch = scene.getDrawBatches()[0];
    REQUIRE(batch._boundsRadius > 0.0f);
    for (uint32_t corner = 0; corner < 8u; ++corner)
    {
        const glm::vec3 point((corner & 1u) ? world._max.x : world._min.x,
                              (corner & 2u) ? world._max.y : world._min.y,
                              (corner & 4u) ? world._max.z : world._min.z);
        CHECK(glm::length(point - batch._boundsCenter) <= batch._boundsRadius + 1e-4f);
    }

    // And tight enough to be worth testing against: a sphere around the whole scene would pass
    // the check above while culling nothing.
    CHECK(batch._boundsCenter.x == doctest::Approx(1.0f));
    CHECK(batch._boundsRadius < glm::length(world.getExtent()));
}
