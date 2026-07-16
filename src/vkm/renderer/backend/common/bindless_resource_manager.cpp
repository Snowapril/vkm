// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/bindless_resource_manager.h>

namespace vkm
{
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
