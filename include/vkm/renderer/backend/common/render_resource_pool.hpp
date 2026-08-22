// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/render_resource_pool.h>

namespace vkm
{
    template <typename ResourceType>
    ResourceType* VkmRenderResourcePool::getResource(VkmResourceHandle handle)
    {
        // See VkmRenderResourcePool::releaseResource() -- an invalid handle carries
        // poolType/type == Undefined, which equals Count and so is one past the end of the
        // arrays they index.
        if (!handle.isValid() || handle.poolType >= VkmResourcePoolType::Count ||
            handle.type >= VkmResourceType::Count)
            return nullptr;

        std::lock_guard<std::mutex> lock(_mutex);
        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)handle.poolType];
        if (handle.id < subPool._resources[(uint8_t)handle.type].size() &&
            subPool._generations[(uint8_t)handle.type][handle.id] == handle.generation)
        {
            return static_cast<ResourceType*>(subPool._resources[(uint8_t)handle.type][handle.id].get());
        }
        return nullptr;
    }
}