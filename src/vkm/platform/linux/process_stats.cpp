// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/process_stats.h>

#include <sys/resource.h>
#include <sys/time.h>

namespace vkm
{
    double getProcessCpuUsagePercent()
    {
        static struct timeval s_previousWallTime{};
        static double s_previousCpuTimeSeconds = -1.0;

        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const double cpuTimeSeconds =
            (usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1'000'000.0) +
            (usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1'000'000.0);

        struct timeval wallTime{};
        gettimeofday(&wallTime, nullptr);

        if (s_previousCpuTimeSeconds < 0.0)
        {
            s_previousWallTime = wallTime;
            s_previousCpuTimeSeconds = cpuTimeSeconds;
            return 0.0;
        }

        const double deltaWallSeconds = (wallTime.tv_sec - s_previousWallTime.tv_sec) +
            (wallTime.tv_usec - s_previousWallTime.tv_usec) / 1'000'000.0;
        const double deltaCpuSeconds = cpuTimeSeconds - s_previousCpuTimeSeconds;

        s_previousWallTime = wallTime;
        s_previousCpuTimeSeconds = cpuTimeSeconds;

        if (deltaWallSeconds <= 0.0)
        {
            return 0.0;
        }
        return (deltaCpuSeconds / deltaWallSeconds) * 100.0;
    }
} // namespace vkm
