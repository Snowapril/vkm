// Copyright (c) 2024 Snowapril

#include <vkm/renderer/engine.h>
#include <vkm/base/logger.hpp>
#include <iostream>

namespace vkm
{
    VkmEngineBase::VkmEngineBase()
    {
    }

    VkmEngineBase::~VkmEngineBase()
    {
    }

    bool VkmEngineBase::initialize()
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

    void VkmEngineBase::runLoop()
    {
    }

    void VkmEngineBase::destroy()
    {
    }
}