// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>

#if defined(VKM_PLATFORM_APPLE)
#include <signal.h>
#endif // defined(VKM_PLATFORM_APPLE)

#if defined(VKM_USE_VULKAN_API) || defined(VKM_USE_METAL_API)
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif // defined(VKM_USE_VULKAN_API) || defined(VKM_USE_METAL_API)

namespace vkm
{
    #if defined(VKM_PLATFORM_WINDOWS)
        #define DEBUG_BREAK __debugbreak()
    #elif defined(VKM_PLATFORM_APPLE)
        #define DEBUG_BREAK raise(SIGTRAP)
    #else 
        #error "Unsupported platform"
    #endif
};