// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    VkmRenderResource::VkmRenderResource(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmRenderResource::~VkmRenderResource()
    {
    }

    void VkmRenderResource::recordUsage(VkmCommandQueueType queueType, VkmGpuEventTimelineObject timelineObject)
    {
        _lastUsagePerQueue[(uint8_t)queueType] = timelineObject;
    }

    VkmGpuEventTimelineObject VkmRenderResource::getLastUsage(VkmCommandQueueType queueType) const
    {
        return _lastUsagePerQueue[(uint8_t)queueType];
    }

    bool VkmRenderResource::hasAnyPendingUsage() const
    {
        for (const VkmGpuEventTimelineObject& usage : _lastUsagePerQueue)
        {
            if (usage._gpuEventTimeline == nullptr)
            {
                continue;
            }
            if (usage._gpuEventTimeline->queryLastCompletedTimeline() < usage._timelineValue)
            {
                return true;
            }
        }
        return false;
    }
} // namespace vkm