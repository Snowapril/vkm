// Copyright (c) 2024 Snowapril

#include <vkm/base/logger.h>

namespace vkm
{
    template <typename... Args>
    void Logger::debug(const char* fmt, Args&&... args)
    {
        _logger->debug(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Logger::info(const char* fmt, Args&&... args)
    {
        _logger->info(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Logger::warn(const char* fmt, Args&&... args)
    {
        _logger->warn(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Logger::error(const char* fmt, Args&&... args)
    {
        _logger->error(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void LoggerManager::debug(const char* fmt, Args&&... args)
    {
        for (auto& logger : _loggers)
        {
            logger->debug(fmt, std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    void LoggerManager::info(const char* fmt, Args&&... args)
    {
        for (auto& logger : _loggers)
        {
            logger->info(fmt, std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    void LoggerManager::warn(const char* fmt, Args&&... args)
    {
        for (auto& logger : _loggers)
        {
            logger->warn(fmt, std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    void LoggerManager::error(const char* fmt, Args&&... args)
    {
        for (auto& logger : _loggers)
        {
            logger->error(fmt, std::forward<Args>(args)...);
        }
    }
}