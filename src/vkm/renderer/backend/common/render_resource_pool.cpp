// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/common/acceleration_structure.h>

namespace vkm
{
    VkmRenderResourcePool::VkmRenderResourcePool(VkmDriverBase* driver)
        : _driver(driver)
    {

    }

    VkmRenderResourcePool::~VkmRenderResourcePool()
    {

    }

    void VkmRenderResourcePool::releaseResource(VkmResourceHandle handle)
    {
        // Guard before any indexing: an invalid handle carries poolType/type values that
        // must not be used as array indices.
        if (!handle.isValid() || handle.poolType >= VkmResourcePoolType::Count ||
            handle.type >= VkmResourceType::Count)
            return;

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
            subPool._freeIds[(uint8_t)handle.type].push_back(handle.id);
        }
    }
    
    void VkmRenderResourcePool::tagResource(VkmResourceHandle handle, VkmResourceMemoryTag tag)
    {
        // Same guard as releaseResource(): Undefined == Count for both enums, so an invalid
        // handle's poolType/type are one past the end of the arrays they would index.
        if (!handle.isValid() || handle.poolType >= VkmResourcePoolType::Count ||
            handle.type >= VkmResourceType::Count)
            return;

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
        // See releaseResource() -- an invalid handle must not be used as an array index.
        if (!handle.isValid() || handle.poolType >= VkmResourcePoolType::Count ||
            handle.type >= VkmResourceType::Count)
            return std::nullopt;

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

    VkmResourceCategoryUsage VkmRenderResourcePool::getAllCategoryMemoryUsage(
        std::array<VkmResourceCategoryUsage, static_cast<size_t>(VkmResourceType::Count)>* outByCategory) const
    {
        VKM_ASSERT(outByCategory != nullptr, "getAllCategoryMemoryUsage requires a destination array");

        outByCategory->fill(VkmResourceCategoryUsage{});
        VkmResourceCategoryUsage total{};

        std::lock_guard<std::mutex> lock(_mutex);
        for (const VkmDriverResourceSubPool& subPool : _subPools)
        {
            for (size_t type = 0; type < subPool._categoryTotals.size(); ++type)
            {
                const VkmResourceCategoryUsage& usage = subPool._categoryTotals[type];
                VkmResourceCategoryUsage& category = (*outByCategory)[type];
                category.totalRequestedBytes += usage.totalRequestedBytes;
                category.totalAllocatedBytes += usage.totalAllocatedBytes;
                category.liveCount += usage.liveCount;
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

    std::vector<VkmResourceHandle> VkmRenderResourcePool::getAllResourceHandles(VkmResourceType type) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<VkmResourceHandle> result;
        for (uint8_t poolType = 0; poolType < (uint8_t)VkmResourcePoolType::Count; ++poolType)
        {
            // A non-null unique_ptr is the liveness truth: releaseResource() resets it, bumps
            // the generation and frees the id. Reading _resources rather than _memoryTags also
            // covers resources that were never tagged (externally-owned swapchain images).
            const std::vector<std::unique_ptr<VkmDriverResourceBase>>& resources =
                _subPools[poolType]._resources[(uint8_t)type];
            const std::vector<VkmResourceHandle::GenerationType>& generations =
                _subPools[poolType]._generations[(uint8_t)type];
            for (size_t id = 0; id < resources.size(); ++id)
            {
                if (resources[id] != nullptr)
                {
                    result.push_back(VkmResourceHandle{ static_cast<VkmResourceHandle::IdType>(id),
                        (VkmResourcePoolType)poolType, type, generations[id] });
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

    VkmResourceHandle VkmRenderResourcePool::allocateAccelerationStructure(VkmAccelerationStructure* accelerationStructure, VkmResourcePoolType poolType)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        VkmResourceHandle handle = allocateResourceLocked(VkmResourceType::AccelerationStructure, poolType);
        _subPools[(uint8_t)poolType]._resources[(uint8_t)VkmResourceType::AccelerationStructure][handle.id].reset(accelerationStructure);
        return handle;
    }

    VkmResourceHandle VkmRenderResourcePool::allocateResourceLocked(VkmResourceType type, VkmResourcePoolType poolType)
    {
        // Caller must already hold _mutex.
        if (poolType == VkmResourcePoolType::Transient && type == VkmResourceType::Texture)
        {
            // Latched, never cleared: it only ever arms beginRenderPass's guard, and a renderer
            // that uses transient attachments at all uses them every frame.
            _hasTransientTextures.store(true, std::memory_order_relaxed);
        }
        if (poolType == VkmResourcePoolType::Aliased && type == VkmResourceType::Texture)
        {
            // Same one-way latch, arming VkmRenderGraph::compile()'s lifetime pass.
            _hasAliasableTextures.store(true, std::memory_order_relaxed);
        }

        VkmDriverResourceSubPool& subPool = _subPools[(uint8_t)poolType];
        std::vector<VkmResourceHandle::IdType>& freeIds = subPool._freeIds[(uint8_t)type];

        VkmResourceHandle::IdType next_id;
        if (!freeIds.empty())
        {
            next_id = freeIds.back();
            freeIds.pop_back();
        }
        else
        {
            next_id = subPool._nextResourceId[(uint8_t)type]++;
            if (next_id >= subPool._resources[(uint8_t)type].size())
            {
                subPool._resources[(uint8_t)type].resize(next_id + VkmDriverResourceSubPool::POOL_GRANURARITY);
                subPool._generations[(uint8_t)type].resize(next_id + VkmDriverResourceSubPool::POOL_GRANURARITY, 0);
                subPool._memoryTags[(uint8_t)type].resize(next_id + VkmDriverResourceSubPool::POOL_GRANURARITY);
            }
        }
        VkmResourceHandle::GenerationType generation = subPool._generations[(uint8_t)type][next_id];
        return VkmResourceHandle{ next_id, poolType, type, generation };
    }

    VkmRenderResourcePool::VkmDriverResourceSubPool::VkmDriverResourceSubPool()
    {

    }

    VkmRenderResourcePool::VkmDriverResourceSubPool::~VkmDriverResourceSubPool()
    {

    }
}
