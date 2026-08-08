// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/render_resource_pool.h>

#include <array>
#include <mutex>
#include <unordered_set>

@protocol MTLResidencySet;
@protocol MTLAllocation;

namespace vkm
{
    class VkmDriverMetal;

    class VkmRenderResourcePoolMetal : public VkmRenderResourcePool
    {
    public:
        VkmRenderResourcePoolMetal(VkmDriverBase* driver);
        ~VkmRenderResourcePoolMetal();

        inline id<MTLResidencySet> getResidencySet(VkmResourcePoolType poolType) const { return _residencySets[(uint8_t)poolType]; }

        /*
        * @brief Flush staged addAllocation/removeAllocation changes with a single commit per set.
        * @details Called by VkmCommandQueueMetal::submit() right before command buffers are
        * committed. A no-op when nothing was staged since the last flush.
        */
        void commitPendingResidencyChanges();

        /*
        * @brief Stage residency for an allocation living outside the resource pool's handle
        * tracking, such as an MTLHeap pool block or the ImGui renderer's raw MTLBuffers.
        * @details A heap block's placed sub-allocations may not make the backing heap resident on
        * their own. Long-lived allocations are never unregistered; allocations that get released
        * and recreated, like ImGui's growing buffers, must call unregisterExternalAllocation before
        * release, the residency set retaining its members.
        * @param allocation Metal allocation to keep resident.
        * @param poolType Which residency set to stage it into.
        */
        void registerExternalAllocation(id<MTLAllocation> allocation, VkmResourcePoolType poolType = VkmResourcePoolType::Default);

        /*
        * @brief Stage removal of a previously registered external allocation.
        * @details Takes effect at the next commitPendingResidencyChanges(). The Metal object must
        * stay valid until that commit; pool resources get this from the deferred reclaimer, and
        * callers of this entry point must release after the frame's submit, or accept that the
        * retained set reference keeps the object alive.
        * @param allocation Metal allocation to drop.
        * @param poolType Which residency set to stage the removal in.
        */
        void unregisterExternalAllocation(id<MTLAllocation> allocation, VkmResourcePoolType poolType = VkmResourcePoolType::Default);

    protected:
        virtual bool initialize() override final;
        virtual void onResourceInitialized(VkmResourceHandle handle) override final;
        virtual void releaseResource(VkmResourceHandle handle) override final;

    private:
        std::array<id<MTLResidencySet>, (uint8_t)VkmResourcePoolType::Count> _residencySets{};
        // Which allocations this pool actually added to each residency set. onResourceInitialized
        // bails out for a resource whose native object does not exist yet -- the swapchain
        // backbuffer, whose drawable arrives later via overrideExternalHandle -- so "the resource
        // has an allocation now" is not evidence that the set ever contained it.
        // Guarded by _residencyMutex.
        std::array<std::unordered_set<const void*>, (uint8_t)VkmResourcePoolType::Count> _residentAllocations;
        std::mutex _residencyMutex;
        bool _residencyDirty{false};  // guarded by _residencyMutex
    };
} // namespace vkm
