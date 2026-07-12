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
        * handle tracking -- e.g. an MTLHeap pool block, whose placed sub-allocations may
        * not make the backing heap resident on their own. Never removed: heap blocks live
        * until driver teardown.
        */
        void registerExternalAllocation(id<MTLAllocation> allocation, VkmResourcePoolType poolType = VkmResourcePoolType::Default);

    protected:
        virtual bool initialize() override final;
        virtual void onResourceInitialized(VkmResourceHandle handle) override final;
        virtual void releaseResource(VkmResourceHandle handle) override final;

    private:
        VkmDriverMetal* _driverMetal;
        std::array<id<MTLResidencySet>, (uint8_t)VkmResourcePoolType::Count> _residencySets{};
        std::mutex _residencyMutex;
        bool _residencyDirty{false}; // guarded by _residencyMutex
    };
} // namespace vkm
