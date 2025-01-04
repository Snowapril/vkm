// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/engine.h>
#include <memory>

namespace vkm
{
    class VkmEngine;

    class VkmWindow
    {
    public:
        VkmWindow();
        ~VkmWindow();

        void create(uint32_t width, uint32_t height, const char* title);
        void destroy();
    };

    class VkmApplication
    {
    public:
        VkmApplication();
        ~VkmApplication();

        int entryPoint(int argc, char* argv[]);

        void destroy();

    private:
        std::unique_ptr<VkmEngine> _engine;
    };
}