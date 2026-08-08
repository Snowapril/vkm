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

    /*
    * @brief Render resource base class
    */
    class VkmRenderResource : public VkmDriverResourceBase
    {
    public:
        VkmRenderResource(VkmDriverBase* driver);
        virtual ~VkmRenderResource();

        inline VkmResourceHandle getHandle() const { return _handle; }
        virtual VkmResourceType getResourceType() const = 0;

        /*
        * @brief Child resources this one owns, such as a texture's views.
        * @details Overridden so VkmDeferredResourceReclaimer can cascade a release: a parent's
        * deferred release blocks until every declared child is gone from the pool too.
        * @return The owned handles, empty by default.
        */
        virtual std::vector<VkmResourceHandle> getOwnedChildHandles() const { return {}; }

        /*
        * @brief Records that this resource was used by a submit.
        * @details Keyed by the timeline's own identity -- each VkmCommandQueueBase owns exactly one
        * VkmGpuEventTimelineBase, so the pointer already identifies the queue instance. Only the
        * latest usage per queue is kept, an earlier submit on the same queue being implied complete
        * once a later one is.
        * @param timelineObject Timeline and value the triggering submit produced.
        */
        void recordUsage(VkmGpuEventTimelineObject timelineObject);
        VkmGpuEventTimelineObject getLastUsage(VkmGpuEventTimelineBase* queueTimeline) const;
        const std::vector<VkmGpuEventTimelineObject>& getAllUsages() const { return _lastUsagePerQueue; }

        /*
        * @brief Non-blocking poll for outstanding GPU work.
        * @return True if any recorded usage's timeline has not completed yet.
        */
        bool hasAnyPendingUsage() const;

        /*
        * @brief Actual GPU-side allocated size and alignment for this resource, if known.
        * @details 0 for types with no independent allocation of their own -- Sampler, TextureView,
        * BufferView -- and where the backend has no introspection API for it.
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