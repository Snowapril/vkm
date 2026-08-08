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
    * @brief Defers destroying a released resource's internal handle until every queue it was
    * recorded as used on has completed that timeline value.
    * @details A dedicated background thread polls pending entries and performs the real
    * VkmRenderResourcePool::releaseResource() once safe. WASM cannot spawn a blocking background
    * thread, so no thread is started there and the same non-blocking sweep runs once per frame via
    * pollOnce(), called from VkmRenderGraph::execute().
    */
    class VkmDeferredResourceReclaimer
    {
    public:
        explicit VkmDeferredResourceReclaimer(VkmDriverBase* driver);
        ~VkmDeferredResourceReclaimer();

        // Starts the background worker thread; a no-op on WASM.
        void start();

        // Stops the worker thread if running and drains all pending entries, blocking until each
        // one's recorded timelines complete. Only runs at shutdown.
        void stop();

        // Replaces a direct VkmRenderResourcePool::releaseResource() call: snapshots the resource's
        // recorded per-queue usage and defers the actual release until safe.
        void requestRelease(VkmResourceHandle handle);

        // Non-blocking sweep of pending entries, releasing any that are now safe. The WASM
        // per-frame fallback entry point, also usable in tests without the real worker thread.
        void pollOnce();

        /*
        * @brief Releases everything currently pending on the calling thread, blocking on each
        * entry's recorded timelines first. What stop() does, without stopping the worker.
        * @details For callers that must not let the worker keep destroying GPU objects in the
        * background -- a test whose next case starts allocating the moment this returns.
        */
        void flushBlocking();

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
