// Copyright (c) 2026 Snowapril

#include <vkm/platform/common/process_stats.h>
#include <vkm/platform/common/process_stats_common.h>

#include <mach/mach.h>
#include <mach/task_info.h>

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

    VkmProcessMemoryStats getProcessMemoryStats()
    {
        VkmProcessMemoryStats stats{};

        task_vm_info_data_t vmInfo{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vmInfo), &count) != KERN_SUCCESS)
        {
            return stats;
        }

        // phys_footprint, not resident_size: it is the figure macOS itself charges the
        // process with (what Activity Monitor shows as Memory), so it matches what a user
        // comparing our numbers against the OS would see.
        stats._residentBytes = static_cast<uint64_t>(vmInfo.phys_footprint);
        stats._virtualBytes = static_cast<uint64_t>(vmInfo.virtual_size);

        // The peak must come from the same ledger as phys_footprint. getrusage's ru_maxrss
        // is a different metric (peak resident pages, excluding compressed and IOKit-mapped
        // memory) and routinely reports a *smaller* number than the current footprint, so
        // mixing the two would show a peak below the live value. The kernel only fills this
        // field from revision 3 of task_vm_info onwards, hence the returned-count check.
        if (count >= TASK_VM_INFO_REV3_COUNT && vmInfo.ledger_phys_footprint_peak > 0)
        {
            stats._peakResidentBytes = static_cast<uint64_t>(vmInfo.ledger_phys_footprint_peak);
        }

        stats._valid = true;
        return stats;
    }
} // namespace vkm
