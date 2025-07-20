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

    void VkmEngine::loopInner(const double currentUpdateTime)
    {
        const double deltaTime = currentUpdateTime - _lastUpdateTime;
        _lastUpdateTime = currentUpdateTime;

        update( deltaTime );
        render( deltaTime );
        
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

    void VkmEngine::update(const double deltaTime)
    {
        _appDelegate->update(deltaTime);
    }

    void VkmEngine::prepareRender()
    {
        // Need to wait gpu
        VkmRenderGraph* renderGraph = _frameRenderGraphs[_currentFrameIndex].get();
        // Ensure the render graph is completed before proceeding
        renderGraph->ensureCompleted();
        // Reset the render graph for the current frame
        renderGraph->reset();
    }

    void VkmEngine::render(const double deltaTime)
    {
        VkmResourceHandle currentBackBuffer = _mainSwapChain->acquireNextImage();
        VKM_DEBUG_INFO(fmt::format("Engine update : delta time : {}", deltaTime).c_str());
        
        VkmRenderGraph* renderGraph = _frameRenderGraphs[_currentFrameIndex].get();
        renderGraph->reset();
        
        _appDelegate->render(renderGraph, currentBackBuffer);

        renderGraph->compile();
        
        renderGraph->execute(VkmRenderGraphCommitOptions{ .waitForCompletion = true } );

        _mainSwapChain->present();
    }

    VkmEngineLaunchOptions VkmEngine::parseEngineLaunchOptions(int argc, char* argv[])
    {
        (void)argc; (void)argv;
        return DEFAULT_ENGINE_LAUNCH_OPTIONS;
    }
}
