// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/memory.h>
#include <vkm/platform/common/process_stats.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <array>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief One coherent sample of everything the engine knows about its memory use, on both
    * sides of the "tracked vs. actual" divide.
    *
    * Tracked: MemoryTracker's per-tag CPU allocations and VkmRenderResourcePool's per-resource
    * GPU totals -- what the engine believes it asked for. Actual: the OS's process figures and
    * the graphics API's device figures -- what someone looking at Activity Monitor or a GPU
    * profiler would see. The gap between them is the point of collecting both.
    *
    * Deliberately free of any ImGui dependency so the shutdown log dump and the unit tests can
    * use the same capture path the inspector window does.
    */
    struct VkmMemorySnapshot
    {
        // --- actual ---
        VkmProcessMemoryStats _process;
        MemoryStats _mimalloc;
        VkmGpuMemoryStats _gpu;

        // --- tracked (CPU) ---
        uint64_t _cpuTrackedRequestedBytes = 0;
        uint64_t _cpuTrackedUsableBytes = 0;
        uint64_t _cpuTrackedLiveCount = 0;
        // Tags with at least one live allocation, sorted by usable bytes descending. Tags are
        // never removed from the tracker, so the fully-freed ones are dropped here instead of
        // padding the list with zero rows.
        std::vector<TaggedAllocationSummary> _cpuTags;

        // --- tracked (GPU) ---
        VkmResourceCategoryUsage _gpuTotal;
        std::array<VkmResourceCategoryUsage, static_cast<size_t>(VkmResourceType::Count)> _gpuByCategory;
    };

    /*
    * @brief Sample every source at once. `driver` may be null (or not yet initialized), in
    * which case only the CPU/process halves are filled in.
    * @details Takes MemoryTracker's global mutex and allocates while copying the tag table, so
    * callers should sample at a low rate rather than per frame.
    */
    VkmMemorySnapshot captureMemorySnapshot(VkmDriverBase* driver);

    // Multi-line VKM_DEBUG_INFO dump of a snapshot -- used at shutdown.
    void logMemoryReport(const VkmMemorySnapshot& snapshot);

    // "412.3 MiB" / "1.02 GiB" / "512 B".
    std::string formatByteSize(uint64_t bytes);

    // "SceneMeshVertices" for a labelled tag, "gltf_importer.cpp:154" for a call-site tag.
    std::string formatMemoryTagName(const TaggedAllocationSummary& tag);
} // namespace vkm
