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

    void VkmRenderResource::recordUsage(VkmGpuEventTimelineObject timelineObject)
    {
        for (VkmGpuEventTimelineObject& existing : _lastUsagePerQueue)
        {
            if (existing._gpuEventTimeline == timelineObject._gpuEventTimeline)
            {
                existing._timelineValue = timelineObject._timelineValue;
                return;
            }
        }
        _lastUsagePerQueue.push_back(timelineObject);
    }

    VkmGpuEventTimelineObject VkmRenderResource::getLastUsage(VkmGpuEventTimelineBase* queueTimeline) const
    {
        for (const VkmGpuEventTimelineObject& usage : _lastUsagePerQueue)
        {
            if (usage._gpuEventTimeline == queueTimeline)
            {
                return usage;
            }
        }
        return VkmGpuEventTimelineObject{};
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