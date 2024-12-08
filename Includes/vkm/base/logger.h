// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/platform.h>
#include <spdlog/spdlog.h>
#include <memory>
#include <vector>
#include <initializer_list>

namespace vkm
{
    /*
    * @brief Logger base class
    * @details wrapper of spdlog logger
    */
    class Logger
    {
    public:
        Logger();
        virtual ~Logger();

        /*
        * @brief Log message with format with debug level
        */
        template <typename... Args>
        void debug(const char* fmt, Args&&... args);
        /*
        * @brief Log message with format with info level
        */
        template <typename... Args>
        void info(const char* fmt, Args&&... args);
        /*
        * @brief Log message with format with warn level
        */
        template <typename... Args>
        void warn(const char* fmt, Args&&... args);
        /*
        * @brief Log message with format with error level
        */
        template <typename... Args>
        void error(const char* fmt, Args&&... args);

        /*
        * @brief setup spdlog common preferences
        */
        bool initializeCommon();

        /*
        * @brief flush log
        */
        void flush();

        /*
        * @brief close logger
        */
        void close();

    protected:
        virtual const char* getLoggerTag() const = 0;

    protected:
        std::shared_ptr<spdlog::logger> _logger;
    };

    /*
    * @brief Console logger
    * @details every log via this logger will be printed to console
    */
    class ConsoleLogger : public Logger
    {
    public:
        ConsoleLogger();
        ~ConsoleLogger();

        /*
        * @brief Initialize console logger
        */
        bool initialize();

    protected:
        inline const char* getLoggerTag() const override final
        {
            return "ConsoleLogger";
        }
    };

    /*
    * @brief File logger
    * @details every log via this logger will be written to file. This logger wiil be flushed every 10ms
    */
    class FileLogger : public Logger
    {
    public:
        FileLogger();
        ~FileLogger();

        /*
        * @brief Initialize file logger
        */
        bool initialize(const char* filename);

    protected:
        inline const char* getLoggerTag() const override final
        {
            return "FileLogger";
        }
    };

    /*
    * @brief Logger manager
    * @details manage multiple loggers lifecycle
    */
    class LoggerManager
    {
    public:
        LoggerManager();
        ~LoggerManager();

        static LoggerManager& singleton();

        /*
        * @brief initialize logger manager with predefined logger types
        */
        bool initialize();

        /*
        * @brief add logger to manager
        */
        void addLogger(std::unique_ptr<Logger>&& logger);

        /*
        * @brief Log message with format with debug level
        */
        template <typename... Args>
        void debug(const char* fmt, Args&&... args);
        /*
        * @brief Log message with format with info level
        */
        template <typename... Args>
        void info(const char* fmt, Args&&... args);
        /*
        * @brief Log message with format with warn level
        */
        template <typename... Args>
        void warn(const char* fmt, Args&&... args);
        /*
        * @brief Log message with format with error level
        */
        template <typename... Args>
        void error(const char* fmt, Args&&... args);

        /*
        * @brief flush all loggers
        */
        void flush();

        /*
        * @brief close all loggers
        */
        void close();

    private:
        std::vector<std::unique_ptr<Logger>> _loggers;
    };
}