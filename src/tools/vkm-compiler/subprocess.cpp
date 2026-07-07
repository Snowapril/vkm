// Copyright (c) 2025 Snowapril

#include "subprocess.h"

#include <array>
#include <cstdio>

#if defined(_WIN32)
#define VKM_POPEN _popen
#define VKM_PCLOSE _pclose
#else
#include <sys/wait.h>
#define VKM_POPEN popen
#define VKM_PCLOSE pclose
#endif

namespace vkm
{
    namespace
    {
        // Wrap a single argument in double quotes, escaping embedded quotes so
        // the shell sees it as one token even when it contains spaces.
        std::string quoteArg(const std::string& arg)
        {
            std::string quoted = "\"";
            for (char c : arg)
            {
                if (c == '"' || c == '\\')
                {
                    quoted.push_back('\\');
                }
                quoted.push_back(c);
            }
            quoted.push_back('"');
            return quoted;
        }
    } // namespace

    SubprocessResult runSubprocess(const std::string& executable,
                                   const std::vector<std::string>& args)
    {
        std::string command = quoteArg(executable);
        for (const std::string& arg : args)
        {
            command.push_back(' ');
            command += quoteArg(arg);
        }
        // Redirect stderr into stdout so a single read captures both streams.
        command += " 2>&1";

        SubprocessResult result;

        FILE* pipe = VKM_POPEN(command.c_str(), "r");
        if (pipe == nullptr)
        {
            result.exitCode = -1;
            result.output = "Failed to launch: " + command;
            return result;
        }

        std::array<char, 4096> buffer{};
        size_t read = 0;
        while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0)
        {
            result.output.append(buffer.data(), read);
        }

        int status = VKM_PCLOSE(pipe);
#if defined(_WIN32)
        result.exitCode = status;
#else
        if (WIFEXITED(status))
        {
            result.exitCode = WEXITSTATUS(status);
        }
        else
        {
            result.exitCode = -1;
        }
#endif
        return result;
    }
} // namespace vkm
