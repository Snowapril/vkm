// Copyright (c) 2024 Snowapril

#include <vkm/platform/windows/application.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>

#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

namespace vkm
{
    VkmWindow::VkmWindow()
    {
    }

    VkmWindow::~VkmWindow()
    {
    }

    void VkmWindow::create(uint32_t width, uint32_t height, const char* title)
    {
        (void)width; (void)height; (void)title;
    }

    void VkmWindow::destroy()
    {
    }

    VkmApplication::VkmApplication()
        : _engine( nullptr )
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        _engine = std::make_unique<VkmEngine>( new VkmDriverVulkan() );
        if ( _engine->initialize() == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize engine");
            return;
        }
    }

    VkmApplication::~VkmApplication()
    {
    }

    int VkmApplication::entryPoint(int argc, char* argv[])
    {
        (void)argc; (void)argv;
        return 0;
    }

    void VkmApplication::destroy()
    {
    }
}