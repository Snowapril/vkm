// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

// Window handle type
#if defined(VKM_PLATFORM_WINDOWS)
#define VKM_WINDOW_HANDLE GLFWwindow*
#elif defined(VKM_PLATFORM_APPLE)
#define VKM_WINDOW_HANDLE CAMetalLayer*
#else
    #error "Unsupported platform"
#endif

namespace vkm
{

    /*
    * @brief Window info
    */
    struct VkmWindowInfo
    {
        uint32_t _width;
        uint32_t _height;
        const char* _title;
        void* _windowHandle;
    };
}