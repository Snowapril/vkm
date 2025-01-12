// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

// Window handle type
#if defined(VKM_PLATFORM_WINDOWS)
    using VkmWindowHandle = struct GLFWwindow*;
#elif defined(VKM_PLATFORM_APPLE)
    using VkmWindowHandle = class CAMetalLayer*;
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
        VkmWindowHandle _windowHandle;
    };
}