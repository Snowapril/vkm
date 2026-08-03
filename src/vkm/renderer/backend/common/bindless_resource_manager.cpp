// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/bindless_resource_manager.h>

namespace vkm
{
    void VkmPushConstantRingAllocator::beginFrame(uint32_t frameSlot)
    {
        // Clamped rather than asserted: an out-of-range slot would index past the ring buffer,
        // and region 0 is always valid. The engine only ever passes a real frame slot.
        const uint32_t slot = (frameSlot < FRAME_BUFFER_COUNT) ? frameSlot : 0u;
        _regionBase = slot * kVkmPushConstantRingEntryCount;
        _cursor = _regionBase;
    }

    uint32_t VkmPushConstantRingAllocator::allocate(bool* outOverflowed)
    {
        const uint32_t used = _cursor - _regionBase;
        if (outOverflowed != nullptr)
        {
            *outOverflowed = (used != 0) && (used % kVkmPushConstantRingEntryCount) == 0;
        }
        ++_cursor;
        return _regionBase + (used % kVkmPushConstantRingEntryCount);
    }

    VkmBindlessSlotAllocator::VkmBindlessSlotAllocator(uint32_t capacity)
        : _capacity(capacity)
    {
    }

    uint32_t VkmBindlessSlotAllocator::allocate()
    {
        if (!_freeSlots.empty())
        {
            uint32_t slot = _freeSlots.back();
            _freeSlots.pop_back();
            return slot;
        }
        return (_nextSlot < _capacity) ? _nextSlot++ : UINT32_MAX;
    }

    void VkmBindlessSlotAllocator::release(uint32_t slot)
    {
        _freeSlots.push_back(slot);
    }
} // namespace vkm
