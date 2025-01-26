// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver_resource_pool.h>

namespace vkm
{
    template <typename ResourceType>
    ResourceType* VkmDriverResourcePool::getResource(VkmResourceHandle handle)
    {
        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)handle.poolType];
        if (handle.id < subPool._resources[(uint8_t)handle.type].size())
        {
            return static_cast<ResourceType*>(subPool._resources[(uint8_t)handle.type][handle.id]);
        }
    }
}