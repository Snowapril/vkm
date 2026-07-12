// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/render_resource_pool.h>

#include <array>
#include <mutex>

@protocol MTLResidencySet;

namespace vkm
{
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

    protected:
        virtual void onResourceInitialized(VkmResourceHandle handle) override final;
        virtual void releaseResource(VkmResourceHandle handle) override final;

    private:
        std::array<id<MTLResidencySet>, (uint8_t)VkmResourcePoolType::Count> _residencySets{};
        std::mutex _residencyMutex;
        bool _residencyDirty{false}; // guarded by _residencyMutex
    };
} // namespace vkm
