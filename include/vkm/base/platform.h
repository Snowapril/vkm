// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>

#if defined(VKM_PLATFORM_APPLE) || defined(VKM_PLATFORM_LINUX)
#include <signal.h>
#endif // defined(VKM_PLATFORM_APPLE) || defined(VKM_PLATFORM_LINUX)

#if defined(VKM_PLATFORM_WASM)
#include <emscripten/emscripten.h>
#endif // defined(VKM_PLATFORM_WASM)

// Vulkan, Metal and WebGPU all use a 0..1 NDC depth range (unlike OpenGL's -1..1), so this
// applies to every backend the engine supports.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

namespace vkm
{
    #if defined(VKM_PLATFORM_WINDOWS)
        #define DEBUG_BREAK __debugbreak()
    #elif defined(VKM_PLATFORM_APPLE)
        #define DEBUG_BREAK raise(SIGTRAP)
    #elif defined(VKM_PLATFORM_WASM)
        #define DEBUG_BREAK emscripten_debugger()
    #elif defined(VKM_PLATFORM_LINUX)
        #define DEBUG_BREAK raise(SIGTRAP)
    #else
        #error "Unsupported platform"
    #endif
};