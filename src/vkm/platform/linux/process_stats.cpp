// Copyright (c) 2026 Snowapril

#include <vkm/platform/common/process_stats.h>
#include <vkm/platform/common/process_stats_common.h>

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>

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

    VkmProcessMemoryStats getProcessMemoryStats()
    {
        VkmProcessMemoryStats stats{};

        // /proc/self/statm: total program size and resident set, both in pages.
        std::FILE* statmFile = std::fopen("/proc/self/statm", "r");
        if (statmFile == nullptr)
        {
            return stats;
        }
        unsigned long virtualPages = 0;
        unsigned long residentPages = 0;
        const int fieldsRead = std::fscanf(statmFile, "%lu %lu", &virtualPages, &residentPages);
        std::fclose(statmFile);
        if (fieldsRead != 2)
        {
            return stats;
        }

        const uint64_t pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        stats._virtualBytes = static_cast<uint64_t>(virtualPages) * pageSize;
        stats._residentBytes = static_cast<uint64_t>(residentPages) * pageSize;

        struct rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0)
        {
            // ru_maxrss is in kilobytes on Linux (bytes on Darwin).
            stats._peakResidentBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
        }

        stats._valid = true;
        return stats;
    }
} // namespace vkm
