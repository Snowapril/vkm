// Copyright (c) 2026 Snowapril

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
        // padding the list with zero rows. Empty when the sample was taken without the tag table;
        // the three totals above are filled either way.
        std::vector<TaggedAllocationSummary> _cpuTags;

        // --- tracked (GPU) ---
        VkmResourceCategoryUsage _gpuTotal;
        std::array<VkmResourceCategoryUsage, static_cast<size_t>(VkmResourceType::Count)> _gpuByCategory;
    };

    /*
    * @brief Sample every source at once. `driver` may be null (or not yet initialized), in
    * which case only the CPU/process halves are filled in.
    * @details Two tiers. Without the tag table the sample is a handful of counter reads and is
    * cheap enough for the frame loop. With it, the tracker's whole tag table is copied under
    * MemoryTracker's global mutex -- which every allocation in the process contends on -- then
    * sorted and symbolized, so ask for it only when something is going to display it.
    * @param driver Backend to read GPU figures from, or null.
    * @param includeTagTable Whether to fill _cpuTags.
    */
    VkmMemorySnapshot captureMemorySnapshot(VkmDriverBase* driver, bool includeTagTable = true);

    // Multi-line VKM_DEBUG_INFO dump of a snapshot -- used at shutdown.
    void logMemoryReport(const VkmMemorySnapshot& snapshot);

    // "412.3 MiB" / "1.02 GiB" / "512 B".
    std::string formatByteSize(uint64_t bytes);

    /*
    * @brief "SceneMeshVertices" for a labelled tag, "gltf_importer.cpp:154" for a call-site tag.
    * @details Address-tagged rows (everything that reached global operator new without a
    * VKM_NEW tag) read from the symbol cache resolveMemoryTagCallSites() fills; an address
    * with no name to show formats as a bare "0x...".
    */
    std::string formatMemoryTagName(const TaggedAllocationSummary& tag);

    /*
    * @brief Symbolizes the call site of every address-tagged row.
    * @details Resolution is in-process (dladdr plus __cxa_demangle) and its results are cached
    * process-wide. Code addresses never move, and an address that cannot be named caches as such,
    * so every address costs at most one resolution for the life of the process. Best-effort: it
    * yields the enclosing function's demangled name where the dynamic symbol table carries one --
    * a static or hidden-visibility function does not appear there -- and falls back to the raw
    * address. Never fails, never throws.
    * @param tags Rows to resolve in place.
    */
    void resolveMemoryTagCallSites(const std::vector<TaggedAllocationSummary>& tags);
} // namespace vkm
