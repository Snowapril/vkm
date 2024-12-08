// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/logger.h>

namespace vkm
{
    #define VKM_DEBUG_LOG(fmt, ...) LoggerManager::singleton().debug(fmt, ##__VA_ARGS__)
    #define VKM_DEBUG_WARN(fmt, ...) LoggerManager::singleton().warn(fmt, ##__VA_ARGS__)
    #define VKM_DEBUG_INFO(fmt, ...) LoggerManager::singleton().info(fmt, ##__VA_ARGS__)
    #define VKM_DEBUG_ERROR(fmt, ...) LoggerManager::singleton().error(fmt, ##__VA_ARGS__)
}