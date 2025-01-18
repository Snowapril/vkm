// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/engine.h>

class MTLDevice;

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

    private:
        class VkmWindowImpl* _impl;
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
        class VkmApplicationImpl* _impl;
        MTLDevice* _mtlDevice;
        VkmEngine _engine;

    };
}