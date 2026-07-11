// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/command_queue.h>

#include <array>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief Defers actually destroying a released resource's internal handle until every
    * queue it was recorded as used on (see VkmRenderResource::recordUsage) has completed
    * that timeline value. A dedicated background thread polls pending entries and performs
    * the real vkm::VkmRenderResourcePool::releaseResource() once safe.
    *
    * On WASM, which cannot spawn a blocking background thread, no thread is started; the
    * same non-blocking sweep is instead driven once per frame via pollOnce(), called from
    * VkmRenderGraph::execute().
    */
    class VkmDeferredResourceReclaimer
    {
    public:
        explicit VkmDeferredResourceReclaimer(VkmDriverBase* driver);
        ~VkmDeferredResourceReclaimer();

        // Starts the background worker thread; no-op on WASM (see class comment).
        void start();
        // Stops the worker thread (if running) and drains all pending entries, blocking
        // until each one's recorded timelines complete -- the one place a blocking wait is
        // acceptable, since this only runs at shutdown.
        void stop();

        // Replaces a direct VkmRenderResourcePool::releaseResource() call: snapshots the
        // resource's recorded per-queue usage and defers the actual release until safe.
        void requestRelease(VkmResourceHandle handle);

        // Non-blocking sweep of pending entries, releasing any that are now safe. This is
        // the WASM per-frame fallback entry point, and is also usable directly in tests
        // without starting the real worker thread.
        void pollOnce();

    private:
        struct PendingEntry
        {
            VkmResourceHandle _handle;
            std::vector<VkmGpuEventTimelineObject> _waitsOn;
            std::vector<VkmResourceHandle> _waitsOnChildren;
        };

        bool isEntryReady(const PendingEntry& entry) const;
        void workerLoop();

    private:
        VkmDriverBase* _driver;

        std::mutex _mutex;
        std::condition_variable _cv;
        std::deque<PendingEntry> _pending;

        std::thread _workerThread;
        std::atomic<bool> _running{false};
    };
} // namespace vkm
