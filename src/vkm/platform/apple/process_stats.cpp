// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/process_stats.h>
#include <vkm/platform/common/process_stats_common.h>

#include <sys/resource.h>
#include <sys/time.h>

namespace vkm
{
    double getProcessCpuUsagePercent()
    {
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const double cpuTimeSeconds =
            (usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1'000'000.0) +
            (usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1'000'000.0);

        struct timeval wallTime{};
        gettimeofday(&wallTime, nullptr);
        const double wallTimeSeconds = wallTime.tv_sec + wallTime.tv_usec / 1'000'000.0;

        return cpuUsagePercentFromSamples(cpuTimeSeconds, wallTimeSeconds);
    }
} // namespace vkm
