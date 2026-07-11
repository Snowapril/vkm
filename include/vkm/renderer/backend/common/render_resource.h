// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>

#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /**
     * @brief Render resource base class
     * @details
     */
    class VkmRenderResource : public VkmDriverResourceBase
    {
    public:
        VkmRenderResource(VkmDriverBase* driver);
        virtual ~VkmRenderResource();

        inline VkmResourceHandle getHandle() const { return _handle; }
        virtual VkmResourceType getResourceType() const = 0;

        /*
        * @brief Resources that own child resources (e.g. a texture owning its views)
        * override this so VkmDeferredResourceReclaimer can cascade a release: a parent's
        * deferred release blocks until every declared child is gone from the pool too.
        * Default: no children.
        */
        virtual std::vector<VkmResourceHandle> getOwnedChildHandles() const { return {}; }

        /*
        * @brief Record that this resource was used, tagged with the timeline value the
        * triggering submit produced. Keyed by the timeline's own identity (each
        * VkmCommandQueueBase owns exactly one VkmGpuEventTimelineBase, so this pointer already
        * uniquely identifies the queue instance -- no separate queue-type/index parameter is
        * needed). Only the latest usage per queue instance is kept -- an earlier submit on the
        * same queue is implied complete once a later one is.
        */
        void recordUsage(VkmGpuEventTimelineObject timelineObject);
        VkmGpuEventTimelineObject getLastUsage(VkmGpuEventTimelineBase* queueTimeline) const;
        const std::vector<VkmGpuEventTimelineObject>& getAllUsages() const { return _lastUsagePerQueue; }

        /*
        * @brief Non-blocking poll: true if any recorded usage's timeline hasn't completed yet.
        */
        bool hasAnyPendingUsage() const;

        /*
        * @brief Actual GPU-side allocated size/alignment for this resource, if known. Safe
        * defaults (0) for types with no independent memory allocation of their own (Sampler,
        * TextureView, BufferView) or where the backend has no introspection API for it
        * (Metal/WebGPU report a best-effort passthrough instead, see their overrides).
        */
        virtual uint64_t getAllocatedSize() const { return 0; }
        virtual uint32_t getMemoryAlignment() const { return 0; }

    protected:
        bool initializeCommon(VkmResourceHandle handle)
        {
            if ( _handle.isValid() == false )
            {
                VKM_DEBUG_ERROR("Invalid resource handle");
                return false;
            }

            _handle = handle;
            return true;
        }

    protected:
        VkmDriverBase* _driver;
        VkmResourceHandle _handle;
        std::vector<VkmGpuEventTimelineObject> _lastUsagePerQueue;
    };
} // namespace vkm