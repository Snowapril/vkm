// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace vkm
{
    /*
    * @brief Debug bookkeeping for the two halves of a split barrier -- an MTLFence updated and
    * waited on, or a VkEvent set and waited on.
    *
    * @details A split barrier fails silently and catastrophically: a wait on a handle nothing ever
    * produces does not error, it hangs the queue, and the symptom is a test that never returns
    * rather than a message naming the cause. This records what each half did so the two mistakes
    * that produce that hang are reported where they happen:
    *
    *   - consuming a handle nothing has produced, which waits forever;
    *   - consuming a handle produced in the same encoder, which waits on work that cannot start
    *     until the wait clears.
    *
    * Handles are opaque -- a fence pointer or an event handle -- so one tracker serves every
    * backend. Compiled to nothing unless VKM_DEBUG is defined; the checks are cheap but they run
    * per encoder per frame, and a release build has no use for them.
    */
    class VkmSplitBarrierTracker
    {
    public:
        // Clears everything. Called when a command buffer begins recording, since handles do not
        // carry across submissions.
        void reset();

        // Opens a new encoder scope. Everything produced from here belongs to it, which is what
        // makes the same-encoder case detectable.
        void beginEncoder();

        // `handle` has been produced (updateFence / setEvent) in the current encoder.
        void recordProduce(uint64_t handle, const char* producerName);

        /*
        * @brief Checks a wait before it is recorded, and reports what would hang.
        * @param handle Handle the consumer is about to wait on.
        * @param consumerName Name used in the message, e.g. the subgraph doing the wait.
        * @return True when the wait can be satisfied. False means it would hang, and the caller is
        * expected to skip it -- reporting and then hanging anyway helps nobody.
        */
        bool checkConsume(uint64_t handle, const char* consumerName) const;

    private:
        struct Production
        {
            uint32_t _encoder = 0;
            std::string _producerName;
        };
        std::unordered_map<uint64_t, Production> _produced;
        uint32_t _currentEncoder = 0;
    };
}
