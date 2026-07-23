// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/engine.h>

@protocol MTLDevice;

#if defined(VKM_USE_VULKAN_API)
struct GLFWwindow;
#endif

namespace vkm
{
    class AppDelegate;

    class VkmWindow
    {
    public:
        VkmWindow();
        ~VkmWindow();

        bool create(uint32_t width, uint32_t height, const char* title);
        void destroy();

#if defined(VKM_USE_VULKAN_API)
        void update();
        bool shouldClose() const;

        inline GLFWwindow* getHandle() const { return _windowHandle; }
#endif

    private:
#if defined(VKM_USE_METAL_API)
        class VkmWindowImpl* _impl;
#elif defined(VKM_USE_VULKAN_API)
        GLFWwindow* _windowHandle;
#endif
    };

    class VkmApplication
    {
    public:
        VkmApplication();
        ~VkmApplication();

        /*
        * @brief Entry point of application
        */
        int entryPoint(AppDelegate* appDelegate, int argc, char* argv[]);

        /*
        * @brief Destroy application
        */
        void destroy();

    private:
#if defined(VKM_USE_METAL_API)
        class VkmApplicationImpl* _impl;
        id<MTLDevice> _mtlDevice;
#elif defined(VKM_USE_VULKAN_API)
        VkmWindow _window;       // main scene window
        VkmWindow _imguiWindow;  // dedicated ImGui window
        const char* _appName;
#endif
        VkmEngine _engine;

    };
}