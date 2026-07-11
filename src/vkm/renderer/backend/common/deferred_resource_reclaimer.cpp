// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#include <vector>

namespace vkm
{
    VkmDeferredResourceReclaimer::VkmDeferredResourceReclaimer(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmDeferredResourceReclaimer::~VkmDeferredResourceReclaimer()
    {
        stop();
    }

    void VkmDeferredResourceReclaimer::start()
    {
#ifdef VKM_PLATFORM_WASM
        // WASM forbids blocking the main thread and cannot spawn a background one the same
        // way; pollOnce() is driven per-frame from VkmRenderGraph::execute() instead.
        return;
#else
        if (_running.exchange(true))
        {
            return; // already running
        }
        _workerThread = std::thread(&VkmDeferredResourceReclaimer::workerLoop, this);
#endif
    }

    void VkmDeferredResourceReclaimer::stop()
    {
        if (_running.exchange(false) == false)
        {
            return; // wasn't running (e.g. WASM, or already stopped)
        }

        _cv.notify_all();
        if (_workerThread.joinable())
        {
            _workerThread.join();
        }

        // Drain remaining entries with a bounded blocking wait -- the one place blocking is
        // acceptable here, since this only runs at shutdown.
        std::deque<PendingEntry> remaining;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            remaining.swap(_pending);
        }

        VkmRenderResourcePool* pool = _driver->getRenderResourcePool();
        for (const PendingEntry& entry : remaining)
        {
            for (const VkmGpuEventTimelineObject& usage : entry._waitsOn)
            {
                if (usage._gpuEventTimeline != nullptr)
                {
                    usage._gpuEventTimeline->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
                }
            }
            pool->releaseResource(entry._handle);
        }
    }

    void VkmDeferredResourceReclaimer::requestRelease(VkmResourceHandle handle)
    {
        VkmRenderResourcePool* pool = _driver->getRenderResourcePool();
        VkmRenderResource* resource = pool->getResource<VkmRenderResource>(handle);
        if (resource == nullptr)
        {
            return; // already released or invalid handle
        }

        PendingEntry entry;
        entry._handle = handle;
        entry._waitsOn = resource->getAllUsages();

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _pending.push_back(entry);
        }
        _cv.notify_one();
    }

    void VkmDeferredResourceReclaimer::pollOnce()
    {
        std::vector<VkmResourceHandle> ready;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _pending.begin();
            while (it != _pending.end())
            {
                if (isEntryReady(*it))
                {
                    ready.push_back(it->_handle);
                    it = _pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Release outside the lock: releaseResource() runs the resource's destructor, which
        // should never happen while holding _mutex.
        VkmRenderResourcePool* pool = _driver->getRenderResourcePool();
        for (VkmResourceHandle handle : ready)
        {
            pool->releaseResource(handle);
        }
    }

    bool VkmDeferredResourceReclaimer::isEntryReady(const PendingEntry& entry) const
    {
        for (const VkmGpuEventTimelineObject& usage : entry._waitsOn)
        {
            if (usage._gpuEventTimeline == nullptr)
            {
                continue;
            }
            if (usage._gpuEventTimeline->queryLastCompletedTimeline() < usage._timelineValue)
            {
                return false;
            }
        }
        return true;
    }

    void VkmDeferredResourceReclaimer::workerLoop()
    {
        while (_running.load(std::memory_order_relaxed))
        {
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _cv.wait_for(lock, std::chrono::milliseconds(4), [this] {
                    return !_running.load(std::memory_order_relaxed) || !_pending.empty();
                });
            }
            if (!_running.load(std::memory_order_relaxed))
            {
                break;
            }
            pollOnce();
        }
    }
} // namespace vkm
