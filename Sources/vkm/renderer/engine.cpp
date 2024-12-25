// Copyright (c) 2024 Snowapril

#include <vkm/renderer/engine.h>
#include <vkm/base/logger.hpp>
#include <iostream>

namespace vkm
{
    VkmEngine::VkmEngine(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmEngine::~VkmEngine()
    {
    }

    bool VkmEngine::initialize()
    {
        bool result = LoggerManager::singleton().initialize();
        if (!result)
        {
            std::cerr << "Failed to initialize logger manager" << std::endl;
            return false;
        }

        VKM_DEBUG_INFO("LoggerManager initialized");

        return true;
    }

    void VkmEngine::runLoop()
    {
    }

    void VkmEngine::destroy()
    {
    }
}