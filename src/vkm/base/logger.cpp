// Copyright (c) 2025 Snowapril

#include <vkm/base/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <iostream>
#include <chrono>
#include <backward.hpp>

namespace vkm
{
    Logger::Logger()
    {

    }

    Logger::~Logger()
    {

    }

    bool Logger::initializeCommon()
    {
        #if defined(VKM_DEBUG)
            _logger->set_level(spdlog::level::debug);
        #else
            _logger->set_level(spdlog::level::info);
        #endif
        
        _logger->set_pattern("[%H:%M:%S %z] [%n] [%^--%L--%$] [thread %t] %v");
        return true;
    }

    void Logger::debug(const char* msg)
    {
        _logger->debug(msg);
    }

    void Logger::info(const char* msg)
    {
        _logger->info(msg);
    }

    void Logger::warn(const char* msg)
    {
        _logger->warn(msg);
    }

    void Logger::error(const char* msg)
    {
        // Note(snowapril) : error log automatically generate additional stack trace
        backward::StackTrace st;
        st.load_here(32);

        backward::TraceResolver tr; 
        tr.load_stacktrace(st);
        
        static thread_local std::ostringstream os;
        os << msg << "\n";
        for (size_t i = 0; i < st.size(); ++i)
        {
            backward::ResolvedTrace trace = tr.resolve(st[i]);
            os << "#" << i
                << " " << trace.object_filename
                << " " << trace.object_function
                << " [" << trace.addr << "]" << "\n";
        }

        _logger->error(os.str().c_str());
        os.clear();
    }

    void Logger::flush()
    {
        _logger->flush();
    }

    void Logger::close()
    {
        _logger.reset();
    }

    ConsoleLogger::ConsoleLogger()
    {

    }

    ConsoleLogger::~ConsoleLogger()
    {

    }

    bool ConsoleLogger::initialize()
    {
        _logger = spdlog::stdout_color_mt(getLoggerTag());
        
        return initializeCommon();
    }

    FileLogger::FileLogger()
    {

    }

    FileLogger::~FileLogger()
    {

    }

    bool FileLogger::initialize(const char* filename)
    {
        try 
        {
            _logger = spdlog::basic_logger_mt( getLoggerTag(), filename);
        }
        catch (const spdlog::spdlog_ex &ex)
        {
            std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
            return false;
        }

        return initializeCommon();
    }
    

    LoggerManager& LoggerManager::singleton()
    {
        static LoggerManager sInstance;
        return sInstance;
    }

    LoggerManager::LoggerManager()
    {

    }

    LoggerManager::~LoggerManager()
    {

    }

    bool LoggerManager::initialize()
    {
        // 1. Add console logger
        auto consoleLogger = std::make_unique<ConsoleLogger>();
        if (!consoleLogger->initialize())
        {
            return false;
        }
        addLogger(std::move(consoleLogger));

        // 2. Add file logger with current time
        auto fileLogger = std::make_unique<FileLogger>();
        // TODO: use std::chrono to get current time and format it to filename
        if (!fileLogger->initialize("vkm.log"))
        {
            return false;
        }
        addLogger(std::move(fileLogger));

        return true;
    }

    void LoggerManager::addLogger(std::unique_ptr<Logger>&& logger)
    {
        _loggers.push_back(std::move(logger));
    }

    void LoggerManager::debug(const char* msg)
    {
        for ( auto& logger : _loggers)
            logger->debug(msg);
    }

    void LoggerManager::info(const char* msg)
    {
        for ( auto& logger : _loggers)
            logger->info(msg);
    }

    void LoggerManager::warn(const char* msg)
    {
        for ( auto& logger : _loggers)
            logger->warn(msg);
    }

    void LoggerManager::error(const char* msg)
    {
        for ( auto& logger : _loggers)
            logger->error(msg);
    }

    void LoggerManager::flush()
    {
        for ( auto& logger : _loggers)
        {
            logger->flush();
        }
    }

    void LoggerManager::close()
    {
        for (auto& logger : _loggers)
        {
            logger->close();
        }
        _loggers.clear();
    }
}