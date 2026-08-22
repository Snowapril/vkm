// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/common/split_barrier_tracker.h>

namespace vkm
{
    void VkmSplitBarrierTracker::reset()
    {
        _produced.clear();
        _currentEncoder = 0;
    }

    void VkmSplitBarrierTracker::beginEncoder()
    {
        ++_currentEncoder;
    }

    void VkmSplitBarrierTracker::recordProduce(uint64_t handle, const char* producerName)
    {
#if defined(VKM_DEBUG)
        Production& production = _produced[handle];
        production._encoder = _currentEncoder;
        production._producerName = producerName != nullptr ? producerName : "<unnamed>";
#else
        (void)handle;
        (void)producerName;
#endif
    }

    bool VkmSplitBarrierTracker::checkConsume(uint64_t handle, const char* consumerName) const
    {
#if defined(VKM_DEBUG)
        const char* consumer = consumerName != nullptr ? consumerName : "<unnamed>";
        const auto it = _produced.find(handle);
        if (it == _produced.end())
        {
            VKM_DEBUG_ERROR(
                (std::string("Split barrier: '") + consumer +
                 "' waits on a handle nothing has produced. The wait can never clear, so the queue "
                 "would hang here rather than report an error.")
                    .c_str());
            return false;
        }
        if (it->second._encoder == _currentEncoder)
        {
            VKM_DEBUG_ERROR(
                (std::string("Split barrier: '") + consumer + "' waits on a handle '" +
                 it->second._producerName +
                 "' produced in this same encoder. The producing work cannot start until the wait "
                 "clears and the wait cannot clear until it starts; use an in-encoder barrier "
                 "instead.")
                    .c_str());
            return false;
        }
        return true;
#else
        (void)handle;
        (void)consumerName;
        return true;
#endif
    }
}
