// Copyright (c) 2025 Snowapril

#pragma once

#include "vkm/base/common.h"

namespace vkm
{
    enum class VkmResourceType : uint8_t
    {
        Texture = 0,
        Buffer = 1,
        StagingBuffer = 2,
        Unknown = 255,
    };

    /*
    * @brief Resource handle
    * @details
    */
    struct VkmResourceHandle
    {
        uint64_t id;
        uint16_t poolIndex;
        VkmResourceType type;

        constexpr const bool operator==(const VkmResourceHandle& other) const
        {
            return id == other.id && poolIndex == other.poolIndex && type == other.type;
        }
        constexpr const bool operator!=(const VkmResourceHandle& other) const
        {
            return id != other.id || poolIndex != other.poolIndex || type != other.type;
        }
    };
    constexpr const VkmResourceHandle VKM_INVALID_RESOURCE_HANDLE{(uint64_t)-1, (uint16_t)-1, VkmResourceType::Unknown};
} // namespace vkm