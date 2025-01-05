// Copyright (c) 2024 Snowapril

#include <vkm/renderer/engine.h>
#include <vkm/renderer/backend/common/driver.h>
#include <iostream>

namespace vkm
{
    VkmEngine::VkmEngine(VkmDriverBase* driver)
        : _driver(driver), _lastUpdateTime(0.0)
    {
    }

    VkmEngine::~VkmEngine()
    {
    }

    bool VkmEngine::initialize(VkmEngineLaunchOptions options)
    {
        bool result = LoggerManager::singleton().initialize();
        if (!result)
        {
            std::cerr << "Failed to initialize logger manager" << std::endl;
            return false;
        }
        VKM_DEBUG_INFO("LoggerManager initialized");

        result = _driver->initialize(&options);
        if (!result)
        {
            VKM_DEBUG_ERROR("Failed to initialize renderer backend driver");
            return false;
        }
        VKM_DEBUG_INFO("Renderer backend driver initialized");

        return true;
    }

    void VkmEngine::update(VkmTexture* backBuffer, const double currentUpdateTime)
    {
        (void)backBuffer;

        const double deltaTime = currentUpdateTime - _lastUpdateTime;
        _lastUpdateTime = currentUpdateTime;
        VKM_DEBUG_INFO(fmt::format("Engine update : delta time : {}", deltaTime).c_str());
    }

    void VkmEngine::destroy()
    {
    }

    VkmEngineLaunchOptions VkmEngine::parseEngineLaunchOptions(int argc, char* argv[])
    {
        (void)argc; (void)argv;
        return DEFAULT_ENGINE_LAUNCH_OPTIONS;
    }
}