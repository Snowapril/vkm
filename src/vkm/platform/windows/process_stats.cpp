// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/process_stats.h>
#include <vkm/platform/common/process_stats_common.h>

#include <windows.h>

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
} // namespace vkm
