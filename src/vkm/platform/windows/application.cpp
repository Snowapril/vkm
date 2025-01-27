// Copyright (c) 2025 Snowapril

#include <vkm/platform/windows/application.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>

#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

namespace vkm
{
    VkmWindow::VkmWindow()
        : _windowHandle( nullptr )
    {
    }

    VkmWindow::~VkmWindow()
    {
    }

    bool VkmWindow::create(uint32_t width, uint32_t height, const char* title)
    {
        _windowHandle = glfwCreateWindow(width, height, title, nullptr, nullptr);
        VKM_ASSERT(_windowHandle, fmt::format("Failed to create window(Width : {}, Height : {}, Title : {})", width, height, title).c_str());

        return true;
    }

    void VkmWindow::destroy()
    {
    }

    void VkmWindow::update()
    {
        glfwPollEvents();
    }

    bool VkmWindow::shouldClose() const 
    { 
        return glfwWindowShouldClose(_windowHandle); 
    }

    VkmApplication::VkmApplication(const char* appName)
        : _engine( new VkmDriverVulkan() ), _appName( appName )
    {
        
    }

    VkmApplication::~VkmApplication()
    {
        destroy();
    }

    int VkmApplication::entryPoint(AppDelegate* appDelegate, int argc, char* argv[])
    {
        if ( _engine.initializeEngine( appDelegate, VkmEngine::parseEngineLaunchOptions(argc, argv)) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize engine");
            return -1;
        }

        VKM_ASSERT(glfwInit(), "Failed to initialize GLFW");
        VKM_ASSERT(glfwVulkanSupported(), "This system does not support Vulkan API");

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        if ( _engine.initializeBackendDriver() == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize backend vulkan api");
            return -1;
        }

        if ( _window.create(  1280, 720, _appName ) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize window");
            return -1;
        }
        else
        {
            VkmWindowInfo windowInfo = { 1280, 720, _appName, _window.getHandle() };
            _engine.addSwapChain(windowInfo);
        }
        
        while (_window.shouldClose() == false)
        {
            _window.update();
        }

        glfwTerminate();
        return 0;
    }

    void VkmApplication::destroy()
    {
        _window.destroy();
        _engine.destroy();
    }
}