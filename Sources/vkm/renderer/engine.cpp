// Copyright (c) 2024 Snowapril

#include <vkm/renderer/engine.h>
#include <vkm/renderer/backend/common/driver.h>
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

        result = _driver->initialize();
        if (!result)
        {
            VKM_DEBUG_ERROR("Failed to initialize renderer backend driver");
            return false;
        }
        VKM_DEBUG_INFO("Renderer backend driver initialized");

        return true;
    }

    void VkmEngine::runLoop()
    {
    }

    void VkmEngine::destroy()
    {
    }
}