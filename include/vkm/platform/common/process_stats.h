// Copyright (c) 2026 Snowapril

#pragma once

#include <cstdint>

namespace vkm
{
    // Current process CPU usage as a percentage of one core (0-100 typically, can exceed
    // 100 when multiple threads are busy) sampled since the previous call. Only implemented
    // on macOS today; other platforms return 0.0.
    double getProcessCpuUsagePercent();

    /*
    * @brief What the operating system says this process occupies, as opposed to what the
    * engine's own allocators think they handed out (see vkm::MemoryTracker for that side).
    * @details The resident figure includes everything: the engine's heap, the binary, thread
    * stacks, mapped files, and driver-side allocations -- on unified-memory devices that
    * includes GPU resources too, so it must not be added to VkmGpuMemoryStats' numbers.
    */
    struct VkmProcessMemoryStats
    {
        uint64_t _residentBytes = 0;     // RSS / working set (macOS: phys_footprint)
        uint64_t _peakResidentBytes = 0; // high-water mark, 0 when the platform can't report it
        uint64_t _virtualBytes = 0;      // reserved address space, 0 when unavailable
        bool _valid = false;             // false when the platform cannot report any of it
    };

    VkmProcessMemoryStats getProcessMemoryStats();
} // namespace vkm
