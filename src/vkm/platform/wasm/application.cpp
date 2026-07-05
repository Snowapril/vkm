// Copyright (c) 2025 Snowapril

#include <vkm/platform/wasm/application.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>

#include <GLFW/glfw3.h>
#include <emscripten/emscripten.h>

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
        : _engine( new VkmDriverWebGPU() )
    {
    }

    VkmApplication::~VkmApplication()
    {
        destroy();
    }

    void VkmApplication::mainLoopTrampoline(void* userData)
    {
        VkmApplication* self = static_cast<VkmApplication*>(userData);
        self->_window.update();
        if (self->_window.shouldClose())
        {
            emscripten_cancel_main_loop();
            self->destroy();
        }
    }

    int VkmApplication::entryPoint(AppDelegate* appDelegate, int argc, char* argv[])
    {
        _appDelegate = appDelegate;

        if ( _engine.initializeEngine( appDelegate, VkmEngine::parseEngineLaunchOptions(argc, argv)) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize engine");
            return -1;
        }

        VKM_ASSERT(glfwInit(), "Failed to initialize GLFW");

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        VkmInitResult driverInitResult = _engine.initializeBackendDriver();
        if ( driverInitResult.code != VkmInitResultCode::Success )
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to initialize backend WebGPU api: {}", driverInitResult.reason).c_str());
            return -1;
        }

        if ( _window.create( 1280, 720, appDelegate->getAppName() ) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize window");
            return -1;
        }
        else
        {
            VkmWindowInfo windowInfo = { 1280, 720, appDelegate->getAppName(), _window.getHandle() };
            _engine.addSwapChain(windowInfo);
        }

        // Emscripten forbids blocking the main thread. Register a callback-driven loop
        // (fired at the browser's requestAnimationFrame cadence) instead of the blocking
        // while() loop the other platforms use. This call does not return in the normal
        // sense -- control unwinds back to the browser event loop -- so any cleanup must
        // happen from mainLoopTrampoline once the window should close, not after this call.
        emscripten_set_main_loop_arg(&VkmApplication::mainLoopTrampoline, this, 0, 1);

        return 0;
    }

    void VkmApplication::destroy()
    {
        _window.destroy();
        _engine.destroy();
        glfwTerminate();
    }
}
