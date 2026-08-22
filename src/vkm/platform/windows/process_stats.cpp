// Copyright (c) 2026 Snowapril

#include <vkm/platform/common/process_stats.h>
#include <vkm/platform/common/process_stats_common.h>

#include <windows.h>
#include <psapi.h>

namespace vkm
{
    namespace
    {
        double filetimeToSeconds(const FILETIME& fileTime)
        {
            ULARGE_INTEGER ticks{};
            ticks.LowPart = fileTime.dwLowDateTime;
            ticks.HighPart = fileTime.dwHighDateTime;
            return ticks.QuadPart / 10'000'000.0; // FILETIME ticks are 100ns units.
        }
    } // namespace

    double getProcessCpuUsagePercent()
    {
        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime);
        const double cpuTimeSeconds = filetimeToSeconds(kernelTime) + filetimeToSeconds(userTime);

        FILETIME wallFileTime{};
        GetSystemTimeAsFileTime(&wallFileTime);
        const double wallTimeSeconds = filetimeToSeconds(wallFileTime);

        return cpuUsagePercentFromSamples(cpuTimeSeconds, wallTimeSeconds);
    }

    VkmProcessMemoryStats getProcessMemoryStats()
    {
        VkmProcessMemoryStats stats{};

        // K32GetProcessMemoryInfo rather than GetProcessMemoryInfo: it is exported from
        // kernel32, so no psapi.lib has to be added to the link line.
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0)
        {
            return stats;
        }

        stats._residentBytes = static_cast<uint64_t>(counters.WorkingSetSize);
        stats._peakResidentBytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
        stats._virtualBytes = static_cast<uint64_t>(counters.PagefileUsage);
        stats._valid = true;
        return stats;
    }
} // namespace vkm
