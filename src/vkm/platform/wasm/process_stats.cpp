// Copyright (c) 2026 Snowapril

#include <vkm/platform/common/process_stats.h>

#include <emscripten/heap.h>

namespace vkm
{
    double getProcessCpuUsagePercent()
    {
        // Not implemented on wasm yet.
        return 0.0;
    }

    VkmProcessMemoryStats getProcessMemoryStats()
    {
        VkmProcessMemoryStats stats{};
        // The WASM linear memory is the whole world here: there is no process RSS to ask
        // for, and the heap size is exactly what the module has taken from the host.
        stats._residentBytes = static_cast<uint64_t>(emscripten_get_heap_size());
        stats._virtualBytes = static_cast<uint64_t>(emscripten_get_heap_max());
        stats._valid = true;
        return stats;
    }
} // namespace vkm
