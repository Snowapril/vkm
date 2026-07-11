// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

#include <array>
#include <vector>
#include <mutex>
#include <optional>

namespace vkm
{
    class VkmDriverBase;
    class VkmTexture;
    class VkmBuffer;
    class VkmStagingBuffer;
    class VkmSampler;
    class VkmTextureView;
    class VkmBufferView;

    class VkmRenderResourcePool
    {
    public:
        VkmRenderResourcePool(VkmDriverBase* driver);
        ~VkmRenderResourcePool();

        template <typename ResourceType>
        ResourceType* getResource(VkmResourceHandle handle);

        VkmResourceHandle allocateTexture(VkmTexture* texture, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateBuffer(VkmBuffer* buffer, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateStagingBuffer(VkmStagingBuffer* stagingBuffer, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateSampler(VkmSampler* sampler, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateTextureView(VkmTextureView* textureView, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateBufferView(VkmBufferView* bufferView, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        void releaseResource(VkmResourceHandle handle);\

        /*
        * @brief Attach memory bookkeeping to an already-allocated handle. Must be called at
        * most once per handle's lifetime, right after a successful initialize() -- not
        * enforced structurally, just an invariant every call site honors.
        */
        void tagResource(VkmResourceHandle handle, VkmResourceMemoryTag tag);

        /*
        * @brief Per-handle tag lookup. Returns nullopt once the handle is released (or if it
        * was never tagged) -- unlike the persistent per-category totals below.
        */
        std::optional<VkmResourceMemoryTag> getResourceMemoryTag(VkmResourceHandle handle) const;

        /*
        * @brief Persistent running totals for one resource category, decremented (not reset)
        * on release -- summed across every pool-type sub-pool.
        */
        VkmResourceCategoryUsage getCategoryMemoryUsage(VkmResourceType type) const;

        /*
        * @brief Persistent running totals summed across every category.
        */
        VkmResourceCategoryUsage getTotalMemoryUsage() const;

        /*
        * @brief Snapshot of every currently-live (tagged, not-yet-released) resource's tag.
        */
        std::vector<VkmResourceMemoryTag> getAllMemoryTags() const;

    private:
        // Caller must already hold _mutex.
        VkmResourceHandle allocateResourceLocked(VkmResourceType type, VkmResourcePoolType poolType);

    public:
        class VkmDriverResourceSubPool
        {
            static constexpr const uint32_t POOL_GRANURARITY = 1024; // Maximum number of resources in a subpool
            friend class VkmRenderResourcePool;
        public:
            VkmDriverResourceSubPool();
            ~VkmDriverResourceSubPool();

        private:
            std::array<std::vector<std::unique_ptr<VkmDriverResourceBase>>, (uint8_t)VkmResourceType::Count> _resources;
            std::array<std::vector<uint32_t>, (uint8_t)VkmResourceType::Count> _generations;
            std::array<std::vector<VkmResourceMemoryTag>, (uint8_t)VkmResourceType::Count> _memoryTags;
            std::array<VkmResourceCategoryUsage, (uint8_t)VkmResourceType::Count> _categoryTotals{};
            uint32_t _nextResourceId[(uint8_t)VkmResourceType::Count] = {0, };
        };

    private:
        VkmDriverBase* _driver;
        std::array<VkmDriverResourceSubPool, (uint8_t)VkmResourcePoolType::Count> _subPools;
        mutable std::mutex _mutex;
    };
}