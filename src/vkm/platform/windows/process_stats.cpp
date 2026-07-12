// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/process_stats.h>

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
        static double s_previousWallTimeSeconds = -1.0;
        static double s_previousCpuTimeSeconds = -1.0;

        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime);
        const double cpuTimeSeconds = filetimeToSeconds(kernelTime) + filetimeToSeconds(userTime);

        FILETIME wallFileTime{};
        GetSystemTimeAsFileTime(&wallFileTime);
        const double wallTimeSeconds = filetimeToSeconds(wallFileTime);

        if (s_previousCpuTimeSeconds < 0.0)
        {
            s_previousWallTimeSeconds = wallTimeSeconds;
            s_previousCpuTimeSeconds = cpuTimeSeconds;
            return 0.0;
        }

        const double deltaWallSeconds = wallTimeSeconds - s_previousWallTimeSeconds;
        const double deltaCpuSeconds = cpuTimeSeconds - s_previousCpuTimeSeconds;

        s_previousWallTimeSeconds = wallTimeSeconds;
        s_previousCpuTimeSeconds = cpuTimeSeconds;

        if (deltaWallSeconds <= 0.0)
        {
            return 0.0;
        }
        return (deltaCpuSeconds / deltaWallSeconds) * 100.0;
    }
} // namespace vkm
