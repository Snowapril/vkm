// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

namespace vkm
{
    // Window handle type
    using VkmWindowHandle = class NSWindow*;

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
        VkmApplication(const char* appName);
        ~VkmApplication();

        int entryPoint(int argc, char* argv[]);

        void destroy();

    private:
        class VkmApplicationImpl* _impl;
    };
}