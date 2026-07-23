// Copyright (c) 2025 Snowapril

#include <vkm/platform/linux/application.h>
#include <vkm/platform/common/glfw_input.h>

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

    VkmApplication::VkmApplication()
        : _engine( new VkmDriverVulkan() )
    {

    }

    VkmApplication::~VkmApplication()
    {
        destroy();
    }

    int VkmApplication::entryPoint(AppDelegate* appDelegate, int argc, char* argv[])
    {
        _appName = appDelegate->getAppName();

        if ( _engine.initializeEngine( appDelegate, VkmEngine::parseEngineLaunchOptions(argc, argv)) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize engine");
            return -1;
        }

        VKM_ASSERT(glfwInit(), "Failed to initialize GLFW");
        VKM_ASSERT(glfwVulkanSupported(), "This system does not support Vulkan API");

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        VkmInitResult driverInitResult = _engine.initializeBackendDriver();
        if ( driverInitResult.code != VkmInitResultCode::Success )
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to initialize backend vulkan api: {}", driverInitResult.reason).c_str());
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
            _engine.addSwapChain(windowInfo, /*isImGuiWindow=*/false);
        }

        // Dedicated ImGui window with its own swapchain. ImGui installs its own GLFW input
        // callbacks on this window during addSwapChain(..., true).
        if ( _imguiWindow.create( 960, 640, "ImGui" ) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize ImGui window");
            return -1;
        }
        else
        {
            VkmWindowInfo imguiWindowInfo = { 960, 640, "ImGui", _imguiWindow.getHandle() };
            _engine.addSwapChain(imguiWindowInfo, /*isImGuiWindow=*/true);
        }

        installGlfwInputCallbacks(_window.getHandle(), &_engine);

        // The app quits when the main window closes; closing the ImGui window is vetoed so its
        // swapchain need not be torn down mid-run.
        while (_window.shouldClose() == false && _engine.shouldExit() == false)
        {
            glfwPollEvents(); // services every window; call once per frame
            if (_imguiWindow.shouldClose())
            {
                glfwSetWindowShouldClose(_imguiWindow.getHandle(), GLFW_FALSE);
            }
            _engine.loopInner(glfwGetTime());
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
