// Copyright (c) 2025 Snowapril

#pragma once

namespace vkm
{
    // Shared CPU-usage-percent computation for the platform process_stats.cpp backends. Each
    // platform gathers its own (cpuTimeSeconds, wallTimeSeconds) sample pair via its native API
    // (getrusage/gettimeofday on linux/apple, GetProcessTimes/GetSystemTimeAsFileTime on
    // windows) and calls this helper to turn it into a percentage delta since the previous call.
    //
    // NOTE: the previous-sample state below is `static` inside this inline function, so it is
    // shared across every translation unit that instantiates it. This is safe here only because
    // exactly one platform's process_stats.cpp is compiled into any given build (linux/apple/
    // windows are mutually exclusive), so there is only ever one call site.
    inline double cpuUsagePercentFromSamples(double cpuTimeSeconds, double wallTimeSeconds)
    {
        static double s_previousWallTimeSeconds = -1.0;
        static double s_previousCpuTimeSeconds = -1.0;

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
