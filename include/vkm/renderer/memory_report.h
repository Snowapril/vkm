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
    * @brief One coherent sample of everything the engine knows about its memory use, on both sides
    * of the tracked-versus-actual divide.
    * @details Tracked is MemoryTracker's per-tag CPU allocations and VkmRenderResourcePool's
    * per-resource GPU totals, what the engine believes it asked for. Actual is the OS's process
    * figures and the graphics API's device figures, what a profiler would see. The gap between them
    * is the point of collecting both.
    * Free of any ImGui dependency, so the shutdown log dump and the unit tests use the same capture
    * path the inspector window does.
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

    /*
    * @brief "SceneMeshVertices" for a labelled tag, "gltf_importer.cpp:154" for a call-site tag.
    * @details Address-tagged rows (everything that reached global operator new without a
    * VKM_NEW tag) read from the symbol cache resolveMemoryTagCallSites() fills; an address
    * that has never been resolved formats as a bare "0x...".
    */
    std::string formatMemoryTagName(const TaggedAllocationSummary& tag);

    /*
    * @brief Symbolizes the call site of every address-tagged row.
    * @details Batched: symbolizing costs one child-process launch per loaded module, so resolving a
    * whole snapshot at once is far cheaper than row by row. Results are cached process-wide and
    * code addresses never move, so repeat calls only pay for addresses seen for the first time.
    * Best-effort: it yields "function (file.cpp:123)" where the platform's symbol tooling and the
    * build's debug info allow it, degrades to the function name alone, and falls back to the raw
    * address. Never fails, never throws.
    * @param tags Rows to resolve in place.
    */
    void resolveMemoryTagCallSites(const std::vector<TaggedAllocationSummary>& tags);
} // namespace vkm
