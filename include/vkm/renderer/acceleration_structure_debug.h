// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    class VkmRenderResourcePool;

    /*
    * @brief One live acceleration structure, described without holding on to it.
    * @details The info is copied rather than pointed to because `VkmDeferredResourceReclaimer`
    * releases resources on its own worker thread, so a structure can be freed between the pool
    * lookup and whatever reads the result. The copied info's `_debugName` is a borrowed
    * `const char*` whose owner may be gone and must not be read; `name` is the durable one.
    */
    struct VkmAccelerationStructureSummary
    {
        VkmResourceHandle handle = VKM_INVALID_RESOURCE_HANDLE;
        VkmAccelerationStructureInfo info;
        std::string name;
        uint64_t allocatedBytes = 0;
    };

    /*
    * @brief Summarizes every acceleration structure the pool holds.
    * @details Names and byte sizes come from the pool's memory tags. Top-level structures sort
    * first, being what a consumer expands from.
    * @param pool Pool to enumerate. Null yields an empty list.
    * @return One entry per live structure.
    */
    std::vector<VkmAccelerationStructureSummary> vkmCollectAccelerationStructures(
        VkmRenderResourcePool* pool);

    /*
    * @brief Finds one summary by handle.
    * @param summaries List to search.
    * @param handle Handle to match.
    * @return The entry, or null when the handle names nothing in the list.
    */
    const VkmAccelerationStructureSummary* vkmFindAccelerationStructure(
        const std::vector<VkmAccelerationStructureSummary>& summaries, VkmResourceHandle handle);

    /*
    * @brief Whether a structure carries usable object-space bounds.
    * @details Both-equal means "never filled" -- see VkmAccelerationStructureInfo.
    * @param info Info to test.
    * @return True when the bounds describe a real box.
    */
    bool vkmHasAccelerationStructureBounds(const VkmAccelerationStructureInfo& info);

    /*
    * @brief One top-level instance resolved against its bottom-level structure's bounds.
    * @details `hasBounds` false means the bottom-level structure recorded none (or has been
    * released), and only `transform`'s translation is meaningful.
    */
    struct VkmAccelerationStructureInstanceBox
    {
        VkmResourceHandle tlas = VKM_INVALID_RESOURCE_HANDLE;
        VkmResourceHandle blas = VKM_INVALID_RESOURCE_HANDLE;
        uint32_t instanceId = 0;
        glm::mat4 transform{ 1.0f };
        glm::vec3 boundsMin{ 0.0f };
        glm::vec3 boundsMax{ 0.0f };
        bool hasBounds = false;
    };

    /*
    * @brief Every instance of every top-level structure in `summaries`.
    * @param summaries List produced by vkmCollectAccelerationStructures.
    * @return One entry per top-level instance, in enumeration order.
    */
    std::vector<VkmAccelerationStructureInstanceBox> vkmCollectInstanceBoxes(
        const std::vector<VkmAccelerationStructureSummary>& summaries);

    /*
    * @brief One wireframe box as the acceleration structure debug vertex shader consumes it.
    * @details Pre-transformed on the CPU into a corner and three edge vectors, so the shader
    * builds a corner with three multiply-adds and needs no matrix. Every member is a vec4
    * because a std140 array element is 16-byte aligned regardless, which leaves the `w` lanes
    * free. Mirrors AsDebugBox in resources/Shaders/as_wireframe.hlsl.
    */
    struct VkmAsDebugBox
    {
        // xyz = world position of the (min, min, min) corner; w = 1.0 when selected.
        glm::vec4 _origin{ 0.0f };
        // xyz = world vector spanning the box's local +X, length = the box's extent on that axis.
        glm::vec4 _edgeX{ 0.0f };
        glm::vec4 _edgeY{ 0.0f };
        glm::vec4 _edgeZ{ 0.0f };
    };
    static_assert(sizeof(VkmAsDebugBox) == 64,
                  "VkmAsDebugBox must match AsDebugBox in as_wireframe.hlsl");

    /*
    * @brief How many boxes the debug overlay's uniform buffer holds.
    * @details 256 * sizeof(VkmAsDebugBox) is 16 KiB, the maxUniformBufferRange Vulkan guarantees
    * on every device. Nothing on VkmDriverBase reports the real limit, so this is sized to the
    * guarantee rather than to the device.
    */
    inline constexpr uint32_t kVkmAsDebugMaxBoxes = 256;

    /*
    * @brief Fills `outBoxes` with the drawable subset of `boxes`.
    * @details An instance with no recorded bounds is skipped: there is no size to outline, and a
    * marker box would assert one the engine does not know. Stops at kVkmAsDebugMaxBoxes.
    * @param boxes Instances produced by vkmCollectInstanceBoxes.
    * @param selected Structure to flag as selected; either a matching top-level or bottom-level
    * handle marks an instance.
    * @param outBoxes Receives the records, cleared first. Must not be null.
    * @return True when every drawable instance fitted, false when the capacity clamped the list.
    */
    bool vkmBuildAsDebugBoxes(const std::vector<VkmAccelerationStructureInstanceBox>& boxes,
                              VkmResourceHandle selected, std::vector<VkmAsDebugBox>* outBoxes);
} // namespace vkm
