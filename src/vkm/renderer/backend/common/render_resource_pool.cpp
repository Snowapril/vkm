// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>

namespace vkm
{
    VkmRenderResourcePool::VkmRenderResourcePool(VkmDriverBase* driver)
    {

    }

    VkmRenderResourcePool::~VkmRenderResourcePool()
    {

    }

    void VkmRenderResourcePool::releaseResource(VkmResourceHandle handle)
    {
        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)handle.poolType];
        if (handle.id < subPool._resources[(uint8_t)handle.type].size())
        {
            subPool._resources[(uint8_t)handle.type][handle.id].reset();
        }
    }

    VkmResourceHandle VkmRenderResourcePool::allocateTexture(VkmTexture* texture, VkmResourcePoolType poolType)
    {
        VkmResourceHandle handle = allocateResource(VkmResourceType::Texture, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::Texture][handle.id].reset(texture);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateResource(VkmResourceType type, VkmResourcePoolType poolType)
    {
        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)poolType];
        uint32_t next_id = subPool._nextResourceId[(uint8_t)type]++;
        if (next_id >= subPool._resources[(uint8_t)type].size())
        {
            subPool._resources[(uint8_t)type].resize(next_id + VkmDriverResourceSubPool::POOL_GRANURARITY);
        }
        return VkmResourceHandle{ next_id, poolType, type };
    }

    VkmRenderResourcePool::VkmDriverResourceSubPool::VkmDriverResourceSubPool()
    {

    }

    VkmRenderResourcePool::VkmDriverResourceSubPool::~VkmDriverResourceSubPool()
    {

    }
}