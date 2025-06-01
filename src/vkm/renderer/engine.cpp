// Copyright (c) 2025 Snowapril

#include <vkm/renderer/engine.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/swapchain.h>
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

    bool VkmEngine::initializeEngine(AppDelegate* appDelegate, VkmEngineLaunchOptions options)
    {
        bool result = LoggerManager::singleton().initialize();
        if (!result)
        {
            std::cerr << "Failed to initialize logger manager" << std::endl;
            return false;
        }
        VKM_DEBUG_INFO("LoggerManager initialized");

        _appDelegate.reset(appDelegate);
        _engineOptions = options;

        for (uint8_t i = 0; i < FRAME_COUNT; ++i)
        {
            _frameRenderGraphs[i] = std::make_unique<VkmRenderGraph>(_driver, i);
        }

        return true;
    }
    
    bool VkmEngine::initializeBackendDriver()
    {
        const bool result = _driver->initialize(&_engineOptions);
        if (!result)
        {
            VKM_DEBUG_ERROR("Failed to initialize renderer backend driver");
            return false;
        }
        VKM_DEBUG_INFO("Renderer backend driver initialized");
        _appDelegate->postDriverReady();

        return true;
    }

    void VkmEngine::update(const double currentUpdateTime)
    {
        const double deltaTime = currentUpdateTime - _lastUpdateTime;
        _lastUpdateTime = currentUpdateTime;

        _appDelegate->update(deltaTime);
        
        VkmResourceHandle currentBackBuffer = _mainSwapChain->acquireNextImage();
        VKM_DEBUG_INFO(fmt::format("Engine update : delta time : {}", deltaTime).c_str());

        VkmRenderGraph* renderGraph = _frameRenderGraphs[_currentFrameIndex].get();
        _appDelegate->render(renderGraph, currentBackBuffer);

        renderGraph->compile();
        //VkmCommandBufferBase* commandBuffer = _driver->newCommandBuffer(VkmCommandBufferType::Graphics, _currentFrameIndex);
        //renderGraph->execute(commandBuffer);

        _mainSwapChain->present();

        _currentFrameIndex = (_currentFrameIndex + 1) % FRAME_COUNT;
    }

    void VkmEngine::destroy()
    {
        if (_mainSwapChain != nullptr)
        {
            delete _mainSwapChain;
            _mainSwapChain = nullptr;
        }
    }

    void VkmEngine::addSwapChain(const VkmWindowInfo& windowInfo)
    {
        VkmSwapChainBase* swapChain = _driver->newSwapChain();
        const bool result = swapChain->initialize(windowInfo);
        VKM_ASSERT(result, "Failed to create swapchain");

        VKM_ASSERT(_mainSwapChain == nullptr, "Main swapchain already exists");
        _mainSwapChain = swapChain;
    }

    VkmEngineLaunchOptions VkmEngine::parseEngineLaunchOptions(int argc, char* argv[])
    {
        (void)argc; (void)argv;
        return DEFAULT_ENGINE_LAUNCH_OPTIONS;
    }
}
