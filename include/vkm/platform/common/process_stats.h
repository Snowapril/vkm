// Copyright (c) 2025 Snowapril

#pragma once

namespace vkm
{
    // Current process CPU usage as a percentage of one core (0-100 typically, can exceed
    // 100 when multiple threads are busy) sampled since the previous call. Only implemented
    // on macOS today; other platforms return 0.0.
    double getProcessCpuUsagePercent();
} // namespace vkm
