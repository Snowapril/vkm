// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

#include <array>
#include <atomic>
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
    class VkmAccelerationStructure;

    class VkmRenderResourcePool
    {
    public:
        VkmRenderResourcePool(VkmDriverBase* driver);
        virtual ~VkmRenderResourcePool();

        template <typename ResourceType>
        ResourceType* getResource(VkmResourceHandle handle);

        VkmResourceHandle allocateTexture(VkmTexture* texture, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateBuffer(VkmBuffer* buffer, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateStagingBuffer(VkmStagingBuffer* stagingBuffer, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateSampler(VkmSampler* sampler, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateTextureView(VkmTextureView* textureView, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateBufferView(VkmBufferView* bufferView, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateAccelerationStructure(VkmAccelerationStructure* accelerationStructure, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        /*
        * @brief Backend setup that must not run before the driver's own device validation has
        * succeeded.
        * @details Called from VkmDriverBase::initialize() right after initializeInner(), before any
        * command queue or resource is created.
        */
        virtual bool initialize() { return true; }

        virtual void onResourceInitialized(VkmResourceHandle handle) {}
        virtual void releaseResource(VkmResourceHandle handle);

        /*
        * @brief Attach memory bookkeeping to an already-allocated handle.
        * @details Must be called at most once per handle's lifetime, right after a successful
        * initialize(). Not enforced structurally.
        * @param handle Handle to tag.
        * @param tag Bookkeeping to attach.
        */
        void tagResource(VkmResourceHandle handle, VkmResourceMemoryTag tag);

        /*
        * @brief Per-handle tag lookup.
        * @param handle Handle to look up.
        * @return The tag, or nullopt once the handle is released or if it was never tagged --
        * unlike the persistent per-category totals below.
        */
        std::optional<VkmResourceMemoryTag> getResourceMemoryTag(VkmResourceHandle handle) const;

        /*
        * @brief Persistent running totals for one resource category, summed across every pool-type
        * sub-pool.
        * @details Decremented on release rather than reset.
        * @param type Category to report.
        * @return That category's totals.
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

        /*
        * @brief Snapshot of every currently-live handle of one resource category, the companion to
        * getAllMemoryTags(), which reports sizes but no handles.
        * @details Handles rather than pointers keep this safe across frames: the caller re-resolves
        * through getResource(), and a slot recycled in the meantime is rejected by the generation
        * check instead of dangling.
        * @param type Category to enumerate.
        * @return Every live handle of that category.
        */
        std::vector<VkmResourceHandle> getAllResourceHandles(VkmResourceType type) const;

        /*
        * @brief Whether a transient texture has ever been allocated from this pool.
        * @details A one-way latch, not a live count: its only consumer is
        * VkmCommandBufferBase::beginRenderPass, whose per-attachment guard is a no-op when no
        * attachment is transient, so an exact count would buy nothing over a lock-free load.
        * False means every render pass can skip that guard entirely.
        */
        inline bool hasTransientTextures() const { return _hasTransientTextures.load(std::memory_order_relaxed); }

    private:
        // Caller must already hold _mutex.
        VkmResourceHandle allocateResourceLocked(VkmResourceType type, VkmResourcePoolType poolType);

    public:
        class VkmDriverResourceSubPool
        {
            static constexpr const uint32_t POOL_GRANURARITY = 1024;  // Resources per subpool
            friend class VkmRenderResourcePool;
        public:
            VkmDriverResourceSubPool();
            ~VkmDriverResourceSubPool();

        private:
            std::array<std::vector<std::unique_ptr<VkmDriverResourceBase>>, (uint8_t)VkmResourceType::Count> _resources;
            std::array<std::vector<VkmResourceHandle::GenerationType>, (uint8_t)VkmResourceType::Count> _generations;
            std::array<std::vector<VkmResourceMemoryTag>, (uint8_t)VkmResourceType::Count> _memoryTags;
            std::array<VkmResourceCategoryUsage, (uint8_t)VkmResourceType::Count> _categoryTotals{};
            VkmResourceHandle::IdType _nextResourceId[(uint8_t)VkmResourceType::Count] = {0, };
            // Ids freed by releaseResource(), reused by allocateResourceLocked() before growing the
            // pool. The slot's generation, bumped on release, is what lets getResource() reject a
            // stale pre-recycle handle.
            std::array<std::vector<VkmResourceHandle::IdType>, (uint8_t)VkmResourceType::Count> _freeIds;
        };

    protected:
        inline VkmDriverBase* getDriver() const { return _driver; }

    private:
        VkmDriverBase* _driver;
        std::array<VkmDriverResourceSubPool, (uint8_t)VkmResourcePoolType::Count> _subPools;
        std::atomic<bool> _hasTransientTextures{false};
        mutable std::mutex _mutex;
    };
}