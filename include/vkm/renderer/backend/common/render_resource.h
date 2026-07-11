// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>

#include <array>

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
        * @brief Record that this resource was used on the given queue, tagged with the
        * timeline value the triggering submit produced. Only the latest usage per queue is
        * kept -- earlier ones are implied complete once the latest one is.
        */
        void recordUsage(VkmCommandQueueType queueType, VkmGpuEventTimelineObject timelineObject);
        VkmGpuEventTimelineObject getLastUsage(VkmCommandQueueType queueType) const;

        /*
        * @brief Non-blocking poll: true if any recorded usage's timeline hasn't completed yet.
        */
        bool hasAnyPendingUsage() const;

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
        std::array<VkmGpuEventTimelineObject, (uint8_t)VkmCommandQueueType::Count> _lastUsagePerQueue{};
    };
} // namespace vkm