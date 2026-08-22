// Copyright (c) 2026 Snowapril
//
// Two things nothing in the build would otherwise catch. The box record is mirrored by hand in
// as_wireframe.hlsl, and the capacity constant is what the whole "16 KiB fits every device"
// argument rests on -- the same reason TestReservoirLayout and TestObjectDataLayout exist. The
// corner arithmetic is asserted here too because it is the one part of the overlay that is pure
// CPU logic, and a wrong edge vector is far cheaper to catch here than by eye on a GPU.

#include <doctest/doctest.h>

#include <vkm/renderer/acceleration_structure_debug.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <vector>

namespace
{
    // Handles are made up: nothing here goes near a driver, which is why the collectors take a
    // summary list rather than a pool. Every field matters -- VkmResourceHandle::operator==
    // compares id, poolType, type and generation -- so both sides of every lookup are built here.
    vkm::VkmResourceHandle makeHandle(uint64_t id)
    {
        vkm::VkmResourceHandle handle{};
        handle.id = id;
        handle.poolType = vkm::VkmResourcePoolType::Undefined;
        handle.type = vkm::VkmResourceType::AccelerationStructure;
        handle.generation = 0;
        return handle;
    }

    vkm::VkmAccelerationStructureSummary makeBlas(uint64_t id, const glm::vec3& boundsMin,
                                                  const glm::vec3& boundsMax)
    {
        vkm::VkmAccelerationStructureSummary summary;
        summary.handle = makeHandle(id);
        summary.info._type = vkm::VkmAccelerationStructureType::BottomLevel;
        summary.info._boundsMin = boundsMin;
        summary.info._boundsMax = boundsMax;
        summary.name = "blas";
        return summary;
    }

    vkm::VkmAccelerationStructureSummary makeTlas(
        uint64_t id, const std::vector<vkm::VkmAccelerationStructureInstance>& instances)
    {
        vkm::VkmAccelerationStructureSummary summary;
        summary.handle = makeHandle(id);
        summary.info._type = vkm::VkmAccelerationStructureType::TopLevel;
        summary.info._instances = instances;
        summary.name = "tlas";
        return summary;
    }

    vkm::VkmAccelerationStructureInstance makeInstance(uint64_t blasId, const glm::mat4& transform,
                                                       uint32_t instanceId = 0)
    {
        vkm::VkmAccelerationStructureInstance instance;
        instance._blas = makeHandle(blasId);
        instance._transform = transform;
        instance._instanceId = instanceId;
        return instance;
    }
} // namespace

TEST_CASE("VkmAsDebugBox - the record the vertex shader reads is four float4s")
{
    // Mirrors AsDebugBox in as_wireframe.hlsl. Four float4s is the only shape that is
    // simultaneously a legal std140 uniform array element (stride a multiple of 16), a natural
    // MSL constant struct, and a WGSL uniform array element.
    CHECK(sizeof(vkm::VkmAsDebugBox) == 64);
    CHECK(offsetof(vkm::VkmAsDebugBox, _origin) == 0);
    CHECK(offsetof(vkm::VkmAsDebugBox, _edgeX) == 16);
    CHECK(offsetof(vkm::VkmAsDebugBox, _edgeY) == 32);
    CHECK(offsetof(vkm::VkmAsDebugBox, _edgeZ) == 48);

    // The capacity claim: 16 KiB is the maxUniformBufferRange every Vulkan device guarantees,
    // and nothing on VkmDriverBase reports the real one. Mirrors VKM_AS_DEBUG_MAX_BOXES.
    CHECK(vkm::kVkmAsDebugMaxBoxes == 256);
    CHECK(vkm::kVkmAsDebugMaxBoxes * sizeof(vkm::VkmAsDebugBox) == 16384);
}

TEST_CASE("vkmCollectInstanceBoxes - resolves instances against their bottom-level bounds")
{
    const std::vector<vkm::VkmAccelerationStructureSummary> summaries = {
        makeTlas(1, { makeInstance(2, glm::mat4(1.0f), 7),
                      makeInstance(3, glm::mat4(1.0f), 8),
                      makeInstance(99, glm::mat4(1.0f), 9) }),
        makeBlas(2, glm::vec3(-1.0f), glm::vec3(1.0f)),
        // Both-equal bounds mean "never filled".
        makeBlas(3, glm::vec3(0.0f), glm::vec3(0.0f)),
    };

    const std::vector<vkm::VkmAccelerationStructureInstanceBox> boxes =
        vkm::vkmCollectInstanceBoxes(summaries);

    // Every instance survives, whether or not its structure had bounds -- the 2D tab draws a
    // marker for the ones that did not.
    REQUIRE(boxes.size() == 3);
    CHECK(boxes[0].instanceId == 7);
    CHECK(boxes[0].hasBounds);
    CHECK(boxes[0].boundsMin == glm::vec3(-1.0f));
    CHECK(boxes[0].boundsMax == glm::vec3(1.0f));

    CHECK(boxes[1].instanceId == 8);
    CHECK_FALSE(boxes[1].hasBounds);

    // An instance naming a structure that is not in the list (released under us) is still listed.
    CHECK(boxes[2].instanceId == 9);
    CHECK_FALSE(boxes[2].hasBounds);
}

TEST_CASE("vkmBuildAsDebugBoxes - a corner is the origin plus the edge vectors it selects")
{
    // A unit box at the origin, rotated a quarter turn about +Y and moved to (10, 0, 0). Under
    // that rotation the box's local +X points along world -Z and its local +Z along world +X,
    // which is exactly what an oriented box has to preserve and a re-fitted AABB would lose.
    const glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    const std::vector<vkm::VkmAccelerationStructureSummary> summaries = {
        makeTlas(1, { makeInstance(2, transform) }),
        makeBlas(2, glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f)),
    };

    std::vector<vkm::VkmAsDebugBox> records;
    CHECK(vkm::vkmBuildAsDebugBoxes(vkm::vkmCollectInstanceBoxes(summaries),
                                    vkm::VKM_INVALID_RESOURCE_HANDLE, &records));
    REQUIRE(records.size() == 1);
    const vkm::VkmAsDebugBox& box = records[0];

    const auto approx = [](const glm::vec3& actual, const glm::vec3& expected) {
        CHECK(actual.x == doctest::Approx(expected.x));
        CHECK(actual.y == doctest::Approx(expected.y));
        CHECK(actual.z == doctest::Approx(expected.z));
    };

    // The (min, min, min) corner is the object-space origin carried through the transform.
    approx(glm::vec3(box._origin), glm::vec3(10.0f, 0.0f, 0.0f));
    // Edge vectors are directions: the translation must not apply to them.
    approx(glm::vec3(box._edgeX), glm::vec3(0.0f, 0.0f, -1.0f));
    approx(glm::vec3(box._edgeY), glm::vec3(0.0f, 2.0f, 0.0f));
    approx(glm::vec3(box._edgeZ), glm::vec3(3.0f, 0.0f, 0.0f));

    // Every edge keeps the extent it spans, which a re-fitted AABB would not guarantee.
    CHECK(glm::length(glm::vec3(box._edgeX)) == doctest::Approx(1.0f));
    CHECK(glm::length(glm::vec3(box._edgeY)) == doctest::Approx(2.0f));
    CHECK(glm::length(glm::vec3(box._edgeZ)) == doctest::Approx(3.0f));

    // The corner the shader builds for index 7 (all three axis bits set) is the far corner.
    const glm::vec3 farCorner = glm::vec3(box._origin) + glm::vec3(box._edgeX) +
                                glm::vec3(box._edgeY) + glm::vec3(box._edgeZ);
    approx(farCorner, glm::vec3(transform * glm::vec4(1.0f, 2.0f, 3.0f, 1.0f)));
}

TEST_CASE("vkmBuildAsDebugBoxes - skips boundless instances, flags the selection, clamps")
{
    SUBCASE("an instance with no bounds contributes no box")
    {
        const std::vector<vkm::VkmAccelerationStructureSummary> summaries = {
            makeTlas(1, { makeInstance(2, glm::mat4(1.0f)) }),
            makeBlas(2, glm::vec3(0.0f), glm::vec3(0.0f)),
        };
        std::vector<vkm::VkmAsDebugBox> records;
        CHECK(vkm::vkmBuildAsDebugBoxes(vkm::vkmCollectInstanceBoxes(summaries),
                                        vkm::VKM_INVALID_RESOURCE_HANDLE, &records));
        CHECK(records.empty());
    }

    SUBCASE("selecting a bottom-level structure marks every instance of it")
    {
        std::vector<vkm::VkmAccelerationStructureInstance> instances = {
            makeInstance(2, glm::mat4(1.0f), 0), makeInstance(3, glm::mat4(1.0f), 1),
            makeInstance(2, glm::mat4(1.0f), 2),
        };
        const std::vector<vkm::VkmAccelerationStructureSummary> summaries = {
            makeTlas(1, instances),
            makeBlas(2, glm::vec3(0.0f), glm::vec3(1.0f)),
            makeBlas(3, glm::vec3(0.0f), glm::vec3(1.0f)),
        };
        const vkm::VkmResourceHandle selected = makeHandle(2);

        std::vector<vkm::VkmAsDebugBox> records;
        CHECK(vkm::vkmBuildAsDebugBoxes(vkm::vkmCollectInstanceBoxes(summaries), selected, &records));
        REQUIRE(records.size() == 3);
        CHECK(records[0]._origin.w == 1.0f);
        CHECK(records[1]._origin.w == 0.0f);
        CHECK(records[2]._origin.w == 1.0f);
    }

    SUBCASE("selecting the top-level structure marks all of its instances")
    {
        const std::vector<vkm::VkmAccelerationStructureSummary> summaries = {
            makeTlas(1, { makeInstance(2, glm::mat4(1.0f)), makeInstance(2, glm::mat4(1.0f)) }),
            makeBlas(2, glm::vec3(0.0f), glm::vec3(1.0f)),
        };
        const vkm::VkmResourceHandle selected = makeHandle(1);

        std::vector<vkm::VkmAsDebugBox> records;
        CHECK(vkm::vkmBuildAsDebugBoxes(vkm::vkmCollectInstanceBoxes(summaries), selected, &records));
        REQUIRE(records.size() == 2);
        CHECK(records[0]._origin.w == 1.0f);
        CHECK(records[1]._origin.w == 1.0f);
    }

    SUBCASE("more instances than the capacity clamp rather than overrun the buffer")
    {
        std::vector<vkm::VkmAccelerationStructureInstance> instances;
        for (uint32_t i = 0; i < vkm::kVkmAsDebugMaxBoxes + 10; ++i)
        {
            instances.push_back(makeInstance(2, glm::mat4(1.0f), i));
        }
        const std::vector<vkm::VkmAccelerationStructureSummary> summaries = {
            makeTlas(1, instances),
            makeBlas(2, glm::vec3(0.0f), glm::vec3(1.0f)),
        };

        std::vector<vkm::VkmAsDebugBox> records;
        // Reports the clamp, which is what makes the overlay log instead of silently truncating.
        CHECK_FALSE(vkm::vkmBuildAsDebugBoxes(vkm::vkmCollectInstanceBoxes(summaries),
                                              vkm::VKM_INVALID_RESOURCE_HANDLE, &records));
        CHECK(records.size() == vkm::kVkmAsDebugMaxBoxes);
    }
}
