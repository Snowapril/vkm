// Copyright (c) 2026 Snowapril

#include <vkm/renderer/acceleration_structure_debug.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#include <algorithm>
#include <optional>

namespace vkm
{
    std::vector<VkmAccelerationStructureSummary> vkmCollectAccelerationStructures(
        VkmRenderResourcePool* pool)
    {
        std::vector<VkmAccelerationStructureSummary> summaries;
        if (pool == nullptr)
        {
            return summaries;
        }

        const std::vector<VkmResourceHandle> handles =
            pool->getAllResourceHandles(VkmResourceType::AccelerationStructure);
        summaries.reserve(handles.size());
        for (VkmResourceHandle handle : handles)
        {
            const VkmAccelerationStructure* structure = pool->getResource<VkmAccelerationStructure>(handle);
            if (structure == nullptr)
            {
                continue;
            }
            VkmAccelerationStructureSummary summary;
            summary.handle = handle;
            summary.info = structure->getAccelerationStructureInfo();
            // The durable copy of the name -- the info's _debugName is a borrowed
            // const char* whose owner may be long gone.
            const std::optional<VkmResourceMemoryTag> tag = pool->getResourceMemoryTag(handle);
            if (tag.has_value() && !tag->name.empty())
            {
                summary.name = tag->name;
                summary.allocatedBytes = tag->allocatedSize;
            }
            else
            {
                summary.name = "<unnamed #" + std::to_string(handle.id) + ">";
            }
            summaries.push_back(std::move(summary));
        }
        // Top-level structures first: they are what a consumer expands from.
        std::stable_sort(summaries.begin(), summaries.end(),
                         [](const VkmAccelerationStructureSummary& a,
                            const VkmAccelerationStructureSummary& b) {
                             return a.info._type == VkmAccelerationStructureType::TopLevel &&
                                    b.info._type != VkmAccelerationStructureType::TopLevel;
                         });
        return summaries;
    }

    const VkmAccelerationStructureSummary* vkmFindAccelerationStructure(
        const std::vector<VkmAccelerationStructureSummary>& summaries, VkmResourceHandle handle)
    {
        const auto found = std::find_if(summaries.begin(), summaries.end(),
                                        [handle](const VkmAccelerationStructureSummary& summary) {
                                            return summary.handle == handle;
                                        });
        return found != summaries.end() ? &(*found) : nullptr;
    }

    bool vkmHasAccelerationStructureBounds(const VkmAccelerationStructureInfo& info)
    {
        return info._boundsMin != info._boundsMax;
    }

    std::vector<VkmAccelerationStructureInstanceBox> vkmCollectInstanceBoxes(
        const std::vector<VkmAccelerationStructureSummary>& summaries)
    {
        std::vector<VkmAccelerationStructureInstanceBox> boxes;
        for (const VkmAccelerationStructureSummary& summary : summaries)
        {
            if (summary.info._type != VkmAccelerationStructureType::TopLevel)
            {
                continue;
            }
            for (const VkmAccelerationStructureInstance& instance : summary.info._instances)
            {
                VkmAccelerationStructureInstanceBox box;
                box.tlas = summary.handle;
                box.blas = instance._blas;
                box.instanceId = instance._instanceId;
                box.transform = instance._transform;

                const VkmAccelerationStructureSummary* blas =
                    vkmFindAccelerationStructure(summaries, instance._blas);
                if (blas != nullptr && vkmHasAccelerationStructureBounds(blas->info))
                {
                    box.boundsMin = blas->info._boundsMin;
                    box.boundsMax = blas->info._boundsMax;
                    box.hasBounds = true;
                }
                boxes.push_back(box);
            }
        }
        return boxes;
    }

    bool vkmBuildAsDebugBoxes(const std::vector<VkmAccelerationStructureInstanceBox>& boxes,
                              VkmResourceHandle selected, std::vector<VkmAsDebugBox>* outBoxes)
    {
        VKM_ASSERT(outBoxes != nullptr, "vkmBuildAsDebugBoxes requires an output vector");

        outBoxes->clear();
        outBoxes->reserve(std::min<size_t>(boxes.size(), kVkmAsDebugMaxBoxes));
        bool fitted = true;
        for (const VkmAccelerationStructureInstanceBox& box : boxes)
        {
            if (!box.hasBounds)
            {
                continue;
            }
            if (outBoxes->size() >= kVkmAsDebugMaxBoxes)
            {
                fitted = false;
                break;
            }

            const glm::vec3 extent = box.boundsMax - box.boundsMin;
            const float selectedFlag =
                (selected.isValid() && (box.blas == selected || box.tlas == selected)) ? 1.0f : 0.0f;

            VkmAsDebugBox record;
            record._origin = box.transform * glm::vec4(box.boundsMin, 1.0f);
            record._origin.w = selectedFlag;
            // Direction vectors, so the translation column must not apply: w = 0.
            record._edgeX = box.transform * glm::vec4(extent.x, 0.0f, 0.0f, 0.0f);
            record._edgeY = box.transform * glm::vec4(0.0f, extent.y, 0.0f, 0.0f);
            record._edgeZ = box.transform * glm::vec4(0.0f, 0.0f, extent.z, 0.0f);
            outBoxes->push_back(record);
        }
        return fitted;
    }
} // namespace vkm
