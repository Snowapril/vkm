// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/engine.h>
#include <memory>

struct GLFWwindow;

namespace vkm
{
    class VkmEngine;

    class VkmWindow
    {
    public:
        VkmWindow();
        ~VkmWindow();

        bool create(uint32_t width, uint32_t height, const char* title);
        void destroy();

        void update();
        bool shouldClose() const;
        
    private:
        GLFWwindow* _windowHandle;
    };

    class VkmApplication
    {
    public:
        VkmApplication(const char* appName);
        ~VkmApplication();

        int entryPoint(int argc, char* argv[]);

        void destroy();

    private:
        VkmEngine _engine;
        VkmWindow _window;

        const char* _appName;
    };
}