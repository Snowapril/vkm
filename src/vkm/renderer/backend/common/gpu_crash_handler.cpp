// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/gpu_crash_handler.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>

#include <cstring>

namespace vkm
{
    namespace
    {
        constexpr const char* MARKER_BUFFER_DEBUG_NAME = "VkmGpuCrashHandler_MarkerBuffer";
        constexpr const char* ONE_BUFFER_DEBUG_NAME = "VkmGpuCrashHandler_OneBuffer";
    }

    VkmGpuCrashHandler::VkmGpuCrashHandler(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    void VkmGpuCrashHandler::ensureMarkerBuffersCreated()
    {
        if (_driver->isGpuCrashDumpEnabled() == false || _markerBuffer != nullptr)
        {
            return;
        }

        VkmStagingBufferInfo oneBufferInfo{};
        oneBufferInfo._flags = VkmResourceCreateInfo::AllowTransferSrc;
        oneBufferInfo._size = sizeof(uint32_t);
        oneBufferInfo._debugName = ONE_BUFFER_DEBUG_NAME;
        _oneBuffer = _driver->newStagingBuffer(oneBufferInfo);
        if (_oneBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create GPU crash handler's constant-1 buffer");
            return;
        }
        const uint32_t oneValue = 1;
        std::memcpy(_oneBuffer->map(), &oneValue, sizeof(uint32_t));
        _oneBuffer->flush(0, sizeof(uint32_t));
        // The GPU reads this buffer as a copy source (writeCompletionMarker); on WebGPU a
        // buffer must be unmapped for the GPU to access it at all. No-op on Vulkan/Metal.
        _oneBuffer->unmap();

        VkmStagingBufferInfo markerBufferInfo{};
        markerBufferInfo._flags = VkmResourceCreateInfo::AllowTransferDst;
        markerBufferInfo._size = static_cast<uint64_t>(FRAME_COUNT) * MAX_SUBGRAPHS_PER_FRAME * sizeof(uint32_t);
        markerBufferInfo._debugName = MARKER_BUFFER_DEBUG_NAME;
        _markerBuffer = _driver->newStagingBuffer(markerBufferInfo);
        if (_markerBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create GPU crash handler's subgraph marker buffer");
            return;
        }
        std::memset(_markerBuffer->map(), 0, static_cast<size_t>(markerBufferInfo._size));
        _markerBuffer->flush(0, markerBufferInfo._size);
        // Same reasoning as _oneBuffer -- the GPU writes into this buffer every subgraph, so it
        // must not stay mapped on WebGPU. clearFrameMarkers()/reportCrash() use
        // writeDirect()/map() respectively to touch it afterward without needing it to stay
        // mapped in between.
        _markerBuffer->unmap();
    }

    VkmResourceHandle VkmGpuCrashHandler::getOrCreateMarkerBuffer()
    {
        ensureMarkerBuffersCreated();
        return _markerBuffer != nullptr ? _markerBuffer->getHandle() : VKM_INVALID_RESOURCE_HANDLE;
    }

    VkmResourceHandle VkmGpuCrashHandler::getOrCreateOneBuffer()
    {
        ensureMarkerBuffersCreated();
        return _oneBuffer != nullptr ? _oneBuffer->getHandle() : VKM_INVALID_RESOURCE_HANDLE;
    }

    uint32_t VkmGpuCrashHandler::getMarkerOffset(uint32_t frameIndex, uint32_t subGraphId) const
    {
        if (subGraphId >= MAX_SUBGRAPHS_PER_FRAME)
        {
            VKM_DEBUG_WARN(fmt::format("Subgraph id {} exceeds MAX_SUBGRAPHS_PER_FRAME ({}); skipping its completion marker",
                subGraphId, MAX_SUBGRAPHS_PER_FRAME).c_str());
            return INVALID_VALUE32;
        }
        return (frameIndex * MAX_SUBGRAPHS_PER_FRAME + subGraphId) * static_cast<uint32_t>(sizeof(uint32_t));
    }

    void VkmGpuCrashHandler::clearFrameMarkers(uint32_t frameIndex)
    {
        if (_driver->isGpuCrashDumpEnabled() == false)
        {
            return;
        }

        ensureMarkerBuffersCreated();
        if (_markerBuffer == nullptr)
        {
            return;
        }

        // No reliable wait exists elsewhere in the live render loop for "this frame slot's
        // prior GPU work is done" (VkmEngine::prepareRender() is dead code, execute()'s
        // waitForCompletion option is currently a no-op) -- this blocking wait belongs to this
        // feature alone, and is only ever paid while enableGpuCrashDump is set.
        _driver->getCommandQueue(VkmCommandQueueType::Graphics, 0)->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);

        static const std::vector<uint32_t> zeros(MAX_SUBGRAPHS_PER_FRAME, 0);
        const uint64_t sliceOffset = static_cast<uint64_t>(frameIndex) * MAX_SUBGRAPHS_PER_FRAME * sizeof(uint32_t);
        _markerBuffer->writeDirect(sliceOffset, zeros.data(), MAX_SUBGRAPHS_PER_FRAME * sizeof(uint32_t));
    }

    void VkmGpuCrashHandler::recordSubmission(VkmCommandQueueBase* queue, const CommandSubmitInfo& submitInfo, VkmGpuEventTimelineObject timelineObject)
    {
        if (_driver->isGpuCrashDumpEnabled() == false)
        {
            return;
        }

        VkmGpuSubmissionBreadcrumb breadcrumb;
        breadcrumb.timelineValue = timelineObject._timelineValue;
        breadcrumb.timeline = timelineObject._gpuEventTimeline;
        breadcrumb.queueName = queue->getQueueName() != nullptr ? queue->getQueueName() : "<unnamed queue>";
        breadcrumb.frameIndex = submitInfo.frameIndex;

        breadcrumb.commandBufferNames.reserve(submitInfo.commandBufferCount);
        for (uint32_t i = 0; i < submitInfo.commandBufferCount; ++i)
        {
            VkmCommandBufferBase* commandBuffer = submitInfo.commandBuffers[i];
            const std::string& debugName = commandBuffer->getDebugName();
            breadcrumb.commandBufferNames.push_back(
                debugName.empty() ? fmt::format("{}#{}", breadcrumb.queueName, i) : debugName);

            const std::vector<uint32_t>& subGraphIds = commandBuffer->getRecordedSubGraphIds();
            breadcrumb.subGraphIds.insert(breadcrumb.subGraphIds.end(), subGraphIds.begin(), subGraphIds.end());
        }

        std::lock_guard<std::mutex> lock(_mutex);
        _breadcrumbs.push_back(std::move(breadcrumb));
        while (_breadcrumbs.size() > MAX_BREADCRUMB_ENTRIES)
        {
            _breadcrumbs.pop_front();
        }
    }

    void VkmGpuCrashHandler::reportCrash(const char* backendName, const std::string& errorCode, const std::string& reason)
    {
        VKM_DEBUG_ERROR(fmt::format("[GPU CRASH][{}] error={} reason={}", backendName, errorCode, reason).c_str());

        std::deque<VkmGpuSubmissionBreadcrumb> snapshot;
        const uint32_t* markers = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            snapshot = _breadcrumbs;
            // Best-effort: on WebGPU this re-maps the buffer (an async round trip that may not
            // resolve cleanly once the device is already lost), so a null result here just
            // means per-subgraph detail is skipped below, not that the whole report fails.
            markers = _markerBuffer != nullptr ? static_cast<const uint32_t*>(_markerBuffer->map()) : nullptr;
        }

        if (snapshot.empty())
        {
            VKM_DEBUG_ERROR("[GPU CRASH] No submission breadcrumbs recorded (launch with --enable-gpu-crash-dump to capture submission history).");
            return;
        }

        // Newest first: the most recently submitted, not-yet-completed work is the most likely
        // culprit for a fresh device-lost/GPU-error condition.
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
        {
            const VkmGpuSubmissionBreadcrumb& breadcrumb = *it;
            const bool completed = breadcrumb.timeline != nullptr
                && breadcrumb.timeline->queryLastCompletedTimeline() >= breadcrumb.timelineValue;

            std::string commandBufferList;
            for (size_t i = 0; i < breadcrumb.commandBufferNames.size(); ++i)
            {
                if (i > 0)
                {
                    commandBufferList += ", ";
                }
                commandBufferList += breadcrumb.commandBufferNames[i];
            }

            VKM_DEBUG_ERROR(fmt::format("[GPU CRASH] queue='{}' timeline={} [{}]: {}",
                breadcrumb.queueName, breadcrumb.timelineValue,
                completed ? "COMPLETED" : "SUSPECT - not yet completed", commandBufferList).c_str());

            if (markers == nullptr)
            {
                continue;
            }

            for (uint32_t subGraphId : breadcrumb.subGraphIds)
            {
                if (subGraphId >= MAX_SUBGRAPHS_PER_FRAME)
                {
                    continue;
                }
                const uint32_t markerIndex = breadcrumb.frameIndex * MAX_SUBGRAPHS_PER_FRAME + subGraphId;
                const bool subGraphCompleted = markers[markerIndex] == 1;
                VKM_DEBUG_ERROR(fmt::format("[GPU CRASH]   subgraph #{} (frame {}): {}",
                    subGraphId, breadcrumb.frameIndex, subGraphCompleted ? "COMPLETED" : "NOT COMPLETED").c_str());
            }
        }
    }
}
