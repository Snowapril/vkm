// Copyright (c) 2025 Snowapril

#pragma once

#include <string>
#include <vector>

namespace vkm
{
    struct SubprocessResult
    {
        int exitCode = -1;
        std::string output; // combined stdout + stderr
    };

    // Runs `executable` with `args`, capturing combined stdout/stderr into
    // result.output and the process exit code into result.exitCode. Arguments
    // are quoted so paths containing spaces are handled correctly. This is a
    // small build-time helper (popen-based), not a perf-critical launcher.
    SubprocessResult runSubprocess(const std::string& executable,
                                   const std::vector<std::string>& args);
} // namespace vkm
