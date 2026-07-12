// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/process_stats.h>

namespace vkm
{
    double getProcessCpuUsagePercent()
    {
        // Not implemented on Windows yet.
        return 0.0;
    }
} // namespace vkm
