// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/buffer_view.h>

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
        std::lock_guard<std::mutex> lock(_mutex);
        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)handle.poolType];
        if (handle.id < subPool._resources[(uint8_t)handle.type].size() &&
            subPool._generations[(uint8_t)handle.type][handle.id] == handle.generation)
        {
            VkmResourceMemoryTag& tag = subPool._memoryTags[(uint8_t)handle.type][handle.id];
            if (tag.type != VkmResourceType::Undefined)
            {
                VkmResourceCategoryUsage& totals = subPool._categoryTotals[(uint8_t)handle.type];
                totals.totalRequestedBytes -= tag.requestedSize;
                totals.totalAllocatedBytes -= tag.allocatedSize;
                totals.liveCount -= 1;
                tag = VkmResourceMemoryTag{};
            }
            subPool._resources[(uint8_t)handle.type][handle.id].reset();
            subPool._generations[(uint8_t)handle.type][handle.id]++;
        }
    }

    void VkmRenderResourcePool::tagResource(VkmResourceHandle handle, VkmResourceMemoryTag tag)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)handle.poolType];
        if (handle.id < subPool._resources[(uint8_t)handle.type].size() &&
            subPool._generations[(uint8_t)handle.type][handle.id] == handle.generation)
        {
            subPool._memoryTags[(uint8_t)handle.type][handle.id] = tag;
            VkmResourceCategoryUsage& totals = subPool._categoryTotals[(uint8_t)handle.type];
            totals.totalRequestedBytes += tag.requestedSize;
            totals.totalAllocatedBytes += tag.allocatedSize;
            totals.liveCount += 1;
        }
    }

    std::optional<VkmResourceMemoryTag> VkmRenderResourcePool::getResourceMemoryTag(VkmResourceHandle handle) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)handle.poolType];
        if (handle.id < subPool._resources[(uint8_t)handle.type].size() &&
            subPool._generations[(uint8_t)handle.type][handle.id] == handle.generation)
        {
            const VkmResourceMemoryTag& tag = subPool._memoryTags[(uint8_t)handle.type][handle.id];
            if (tag.type != VkmResourceType::Undefined)
            {
                return tag;
            }
        }
        return std::nullopt;
    }

    VkmResourceCategoryUsage VkmRenderResourcePool::getCategoryMemoryUsage(VkmResourceType type) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceCategoryUsage total{};
        for (const VkmDriverResourceSubPool& subPool : _subPools)
        {
            const VkmResourceCategoryUsage& usage = subPool._categoryTotals[(uint8_t)type];
            total.totalRequestedBytes += usage.totalRequestedBytes;
            total.totalAllocatedBytes += usage.totalAllocatedBytes;
            total.liveCount += usage.liveCount;
        }
        return total;
    }

    VkmResourceCategoryUsage VkmRenderResourcePool::getTotalMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceCategoryUsage total{};
        for (const VkmDriverResourceSubPool& subPool : _subPools)
        {
            for (const VkmResourceCategoryUsage& usage : subPool._categoryTotals)
            {
                total.totalRequestedBytes += usage.totalRequestedBytes;
                total.totalAllocatedBytes += usage.totalAllocatedBytes;
                total.liveCount += usage.liveCount;
            }
        }
        return total;
    }

    std::vector<VkmResourceMemoryTag> VkmRenderResourcePool::getAllMemoryTags() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<VkmResourceMemoryTag> result;
        for (const VkmDriverResourceSubPool& subPool : _subPools)
        {
            for (const auto& tagsForType : subPool._memoryTags)
            {
                for (const VkmResourceMemoryTag& tag : tagsForType)
                {
                    if (tag.type != VkmResourceType::Undefined)
                    {
                        result.push_back(tag);
                    }
                }
            }
        }
        return result;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateTexture(VkmTexture* texture, VkmResourcePoolType poolType)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceHandle handle = allocateResourceLocked(VkmResourceType::Texture, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::Texture][handle.id].reset(texture);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateBuffer(VkmBuffer* buffer, VkmResourcePoolType poolType)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceHandle handle = allocateResourceLocked(VkmResourceType::Buffer, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::Buffer][handle.id].reset(buffer);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateStagingBuffer(VkmStagingBuffer* stagingBuffer, VkmResourcePoolType poolType)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceHandle handle = allocateResourceLocked(VkmResourceType::StagingBuffer, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::StagingBuffer][handle.id].reset(stagingBuffer);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateSampler(VkmSampler* sampler, VkmResourcePoolType poolType)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceHandle handle = allocateResourceLocked(VkmResourceType::Sampler, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::Sampler][handle.id].reset(sampler);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateTextureView(VkmTextureView* textureView, VkmResourcePoolType poolType)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceHandle handle = allocateResourceLocked(VkmResourceType::TextureView, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::TextureView][handle.id].reset(textureView);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateBufferView(VkmBufferView* bufferView, VkmResourcePoolType poolType)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceHandle handle = allocateResourceLocked(VkmResourceType::BufferView, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::BufferView][handle.id].reset(bufferView);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateResourceLocked(VkmResourceType type, VkmResourcePoolType poolType)
    {
        // Caller must already hold _mutex.
        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)poolType];
        uint32_t next_id = subPool._nextResourceId[(uint8_t)type]++;
        if (next_id >= subPool._resources[(uint8_t)type].size())
        {
            subPool._resources[(uint8_t)type].resize(next_id + VkmDriverResourceSubPool::POOL_GRANURARITY);
            subPool._generations[(uint8_t)type].resize(next_id + VkmDriverResourceSubPool::POOL_GRANURARITY, 0);
            subPool._memoryTags[(uint8_t)type].resize(next_id + VkmDriverResourceSubPool::POOL_GRANURARITY);
        }
        uint32_t generation = subPool._generations[(uint8_t)type][next_id];
        return VkmResourceHandle{ next_id, poolType, type, generation };
    }

    VkmRenderResourcePool::VkmDriverResourceSubPool::VkmDriverResourceSubPool()
    {

    }

    VkmRenderResourcePool::VkmDriverResourceSubPool::~VkmDriverResourceSubPool()
    {

    }
}