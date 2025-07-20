// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/platform.h>
#include <vkm/base/logger.h>

namespace vkm
{
    #define VKM_DEBUG_LOG(msg) LoggerManager::singleton().debug(msg)
    #define VKM_DEBUG_WARN(msg) LoggerManager::singleton().warn(msg)
    #define VKM_DEBUG_INFO(msg) LoggerManager::singleton().info(msg)
    #define VKM_DEBUG_ERROR(msg) LoggerManager::singleton().error(msg)
    #define VKM_ASSERT(condition, msg) if (!(condition)) { VKM_DEBUG_ERROR(msg); DEBUG_BREAK; }

    constexpr const uint8_t  INVALID_VALUE8  = 0xFF;
    constexpr const uint16_t INVALID_VALUE16 = 0xFFFF;
    constexpr const uint32_t INVALID_VALUE32 = 0xFFFFFFFF;
    constexpr const uint64_t INVALID_VALUE64 = 0xFFFFFFFFFFFFFFFF;

    constexpr const uint8_t  FRAME_COUNT = 3; // Triple buffering
    constexpr const uint64_t MAX_GPU_TIMEOUT_PER_FRAME = 300; // 300ms seconds per frame
}
