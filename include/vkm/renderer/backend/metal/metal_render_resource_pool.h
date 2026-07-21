// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/render_resource_pool.h>

#include <array>
#include <mutex>

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
        * @brief Flush staged addAllocation/removeAllocation changes with a single commit per
        * set. Called by VkmCommandQueueMetal::submit() right before command buffers are
        * committed; no-op when nothing was staged since the last flush.
        */
        void commitPendingResidencyChanges();

        /*
        * @brief Stage residency for an allocation that lives outside the resource pool's
        * handle tracking -- e.g. an MTLHeap pool block (whose placed sub-allocations may
        * not make the backing heap resident on their own) or the ImGui renderer's raw
        * MTLBuffers/MTLTextures. Long-lived allocations (heap blocks) are simply never
        * unregistered; allocations that get released and recreated (ImGui's growing
        * buffers) must call unregisterExternalAllocation before release, since the
        * residency set retains its members.
        */
        void registerExternalAllocation(id<MTLAllocation> allocation, VkmResourcePoolType poolType = VkmResourcePoolType::Default);

        /*
        * @brief Stage removal of a previously registered external allocation (mirror of
        * registerExternalAllocation). The removal takes effect at the next
        * commitPendingResidencyChanges(); like releaseResource, this only stages -- the
        * deferred flush means the Metal object must stay valid until the pending change
        * commits, which VkmDeferredResourceReclaimer-style delayed release already ensures
        * for pool resources and callers must ensure for external ones (release after the
        * frame's submit, or accept that the retained set reference keeps it alive).
        */
        void unregisterExternalAllocation(id<MTLAllocation> allocation, VkmResourcePoolType poolType = VkmResourcePoolType::Default);

        /*
        * @brief Track the swapchain layer's residency set (CAMetalLayer.residencySet) so the
        * manager is the single owner of every residency set the engine attaches to queues.
        * Unlike the pool-owned _residencySets, this set is owned and auto-updated by Core
        * Animation as drawables rotate; it is only referenced here and is never committed or
        * otherwise modified (Apple forbids modifying it). VkmSwapChainMetal registers it here
        * and attaches it to the backbuffer queue; pass nil to clear on swapchain teardown.
        */
        void setSwapChainResidencySet(id<MTLResidencySet> residencySet);
        inline id<MTLResidencySet> getSwapChainResidencySet() const { return _swapChainResidencySet; }

    protected:
        virtual bool initialize() override final;
        virtual void onResourceInitialized(VkmResourceHandle handle) override final;
        virtual void releaseResource(VkmResourceHandle handle) override final;

    private:
        VkmDriverMetal* _driverMetal;
        std::array<id<MTLResidencySet>, (uint8_t)VkmResourcePoolType::Count> _residencySets{};
        std::mutex _residencyMutex;
        bool _residencyDirty{false}; // guarded by _residencyMutex
        // CAMetalLayer-owned residency set; tracked (not committed) -- see setSwapChainResidencySet.
        id<MTLResidencySet> _swapChainResidencySet{};
    };
} // namespace vkm
