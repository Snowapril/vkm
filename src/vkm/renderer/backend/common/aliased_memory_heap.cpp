// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/aliased_memory_heap.h>
#include <vkm/renderer/backend/common/driver.h>

#include <algorithm>

namespace vkm
{
    namespace
    {
        uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return alignment <= 1 ? value : ((value + alignment - 1) / alignment) * alignment;
        }

        bool rangesOverlap(uint64_t offsetA, uint64_t sizeA, uint64_t offsetB, uint64_t sizeB)
        {
            return offsetA < offsetB + sizeB && offsetB < offsetA + sizeA;
        }
    }

    VkmAliasedMemoryHeap::VkmAliasedMemoryHeap(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmAliasedMemoryHeap::~VkmAliasedMemoryHeap()
    {
        destroy();
    }

    void VkmAliasedMemoryHeap::destroy()
    {
        for (uint32_t blockIndex = 0; blockIndex < (uint32_t)_blocks.size(); ++blockIndex)
        {
            _driver->onDestroyAliasBlock(blockIndex);
        }
        _blocks.clear();
        _registrations.clear();
    }

    VkmAliasedMemoryHeap::Registration* VkmAliasedMemoryHeap::findRegistration(VkmResourceHandle handle)
    {
        const auto it = std::find_if(_registrations.begin(), _registrations.end(),
                                     [handle](const Registration& r) { return r._handle == handle; });
        return it == _registrations.end() ? nullptr : &(*it);
    }

    const VkmAliasedMemoryHeap::Registration* VkmAliasedMemoryHeap::findRegistration(VkmResourceHandle handle) const
    {
        const auto it = std::find_if(_registrations.begin(), _registrations.end(),
                                     [handle](const Registration& r) { return r._handle == handle; });
        return it == _registrations.end() ? nullptr : &(*it);
    }

    bool VkmAliasedMemoryHeap::registerResource(VkmResourceHandle handle, uint64_t sizeBytes, uint64_t alignment,
                                                uint32_t memoryTypeBits)
    {
        if (sizeBytes == 0 || memoryTypeBits == 0)
        {
            VKM_DEBUG_ERROR("An aliasable resource needs a non-zero size and at least one usable memory type");
            return false;
        }
        if (findRegistration(handle) != nullptr)
        {
            VKM_DEBUG_ERROR("An aliasable resource was registered twice");
            return false;
        }

        Registration registration{};
        registration._handle = handle;
        registration._size = sizeBytes;
        registration._alignment = std::max<uint64_t>(alignment, 1);
        registration._memoryTypeBits = memoryTypeBits;
        _registrations.push_back(registration);
        return true;
    }

    void VkmAliasedMemoryHeap::unregisterResource(VkmResourceHandle handle)
    {
        const Registration* registration = findRegistration(handle);
        if (registration == nullptr)
        {
            return;
        }

        // Free the range before dropping the record, so a later placement can reuse it. Blocks
        // themselves are never freed -- they are append-only and die with the driver.
        if (registration->_placement.has_value())
        {
            std::vector<Occupant>& occupants = _blocks[registration->_placement->_blockIndex]._occupants;
            occupants.erase(std::remove_if(occupants.begin(), occupants.end(),
                                           [handle](const Occupant& o) { return o._handle == handle; }),
                            occupants.end());
        }

        _registrations.erase(std::remove_if(_registrations.begin(), _registrations.end(),
                                            [handle](const Registration& r) { return r._handle == handle; }),
                             _registrations.end());
    }

    VkmAliasLifetime VkmAliasedMemoryHeap::lifetimeOf(const std::vector<VkmAliasLifetime>& lifetimes,
                                                      VkmResourceHandle handle, uint32_t subGraphCount)
    {
        const auto it = std::find_if(lifetimes.begin(), lifetimes.end(),
                                     [handle](const VkmAliasLifetime& l) { return l._handle == handle; });
        if (it != lifetimes.end())
        {
            return *it;
        }
        // Undeclared this frame: conservatively live throughout. See the header for why the
        // alternative (an empty lifetime) is unsound.
        return VkmAliasLifetime{handle, 0, subGraphCount == 0 ? 0 : subGraphCount - 1};
    }

    bool VkmAliasedMemoryHeap::tryPlaceInBlock(Block& block, uint32_t blockIndex, const Registration& registration,
                                               const VkmAliasLifetime& lifetime,
                                               const std::vector<VkmAliasLifetime>& lifetimes,
                                               uint32_t subGraphCount, VkmAliasPlacement* outPlacement) const
    {
        // A block's memory type is fixed by whoever landed in it first; an image whose
        // requirements exclude it has to go elsewhere. On Metal every mask is ~0u so this never
        // rejects.
        if ((block._memoryTypeBits & registration._memoryTypeBits) == 0)
        {
            return false;
        }

        // Every aligned end is a candidate, plus the start of the block. Sorted and deduplicated
        // so the lowest usable offset always wins and the result cannot depend on occupant order.
        std::vector<uint64_t> candidates;
        candidates.reserve(block._occupants.size() + 1);
        candidates.push_back(0);
        for (const Occupant& occupant : block._occupants)
        {
            candidates.push_back(alignUp(occupant._offset + occupant._size, registration._alignment));
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        for (uint64_t offset : candidates)
        {
            if (offset + registration._size > block._size)
            {
                continue;
            }

            // The whole rule, in one loop: bytes may be shared only with occupants whose
            // lifetimes cannot coexist with this one.
            const bool conflicts = std::any_of(
                block._occupants.begin(), block._occupants.end(),
                [&](const Occupant& occupant) {
                    if (!rangesOverlap(offset, registration._size, occupant._offset, occupant._size))
                    {
                        return false;
                    }
                    return lifetime.overlaps(lifetimeOf(lifetimes, occupant._handle, subGraphCount));
                });
            if (conflicts)
            {
                continue;
            }

            outPlacement->_blockIndex = blockIndex;
            outPlacement->_offset = offset;
            outPlacement->_size = registration._size;
            outPlacement->_alignment = registration._alignment;
            return true;
        }
        return false;
    }

    bool VkmAliasedMemoryHeap::place(const std::vector<VkmAliasLifetime>& lifetimes,
                                     std::vector<VkmResourceHandle>* outNewlyPlaced)
    {
        uint32_t subGraphCount = 0;
        for (const VkmAliasLifetime& lifetime : lifetimes)
        {
            subGraphCount = std::max(subGraphCount, lifetime._last + 1);
        }

        // Largest first, with the handle id breaking ties. Determinism is load-bearing rather
        // than tidy: the three per-frame-slot graphs compile independently and must agree, and
        // the unit test asserts placements are stable under a shuffled input order.
        std::vector<VkmResourceHandle> pending;
        for (const Registration& registration : _registrations)
        {
            if (!registration._placement.has_value())
            {
                pending.push_back(registration._handle);
            }
        }
        std::sort(pending.begin(), pending.end(), [this](VkmResourceHandle lhs, VkmResourceHandle rhs) {
            const Registration* a = findRegistration(lhs);
            const Registration* b = findRegistration(rhs);
            if (a->_size != b->_size)
            {
                return a->_size > b->_size;
            }
            return a->_handle.id < b->_handle.id;
        });

        bool allPlaced = true;
        for (VkmResourceHandle handle : pending)
        {
            Registration* registration = findRegistration(handle);
            const VkmAliasLifetime lifetime = lifetimeOf(lifetimes, handle, subGraphCount);

            VkmAliasPlacement placement{};
            bool placed = false;
            for (uint32_t blockIndex = 0; blockIndex < (uint32_t)_blocks.size(); ++blockIndex)
            {
                if (tryPlaceInBlock(_blocks[blockIndex], blockIndex, *registration, lifetime, lifetimes,
                                    subGraphCount, &placement))
                {
                    placed = true;
                    break;
                }
            }

            if (!placed)
            {
                // A resource larger than the standard block gets one sized to it rather than
                // failing -- the same shape VkmDriverVulkan::allocateFromBufferPool refuses, but
                // an attachment is a single indivisible allocation so refusing would be fatal.
                const uint64_t blockSize = std::max(BLOCK_SIZE_BYTES, alignUp(registration->_size,
                                                                              registration->_alignment));
                const uint32_t blockIndex = (uint32_t)_blocks.size();
                if (!_driver->onCreateAliasBlock(blockIndex, blockSize, registration->_memoryTypeBits))
                {
                    VKM_DEBUG_ERROR("Failed to create an aliasing memory block");
                    allPlaced = false;
                    continue;
                }

                Block block{};
                block._size = blockSize;
                block._memoryTypeBits = registration->_memoryTypeBits;
                _blocks.push_back(block);

                placement._blockIndex = blockIndex;
                placement._offset = 0;
                placement._size = registration->_size;
                placement._alignment = registration->_alignment;
            }

            registration->_placement = placement;
            _blocks[placement._blockIndex]._occupants.push_back(
                Occupant{handle, placement._offset, placement._size});
            if (outNewlyPlaced != nullptr)
            {
                outNewlyPlaced->push_back(handle);
            }
        }

        return allPlaced;
    }

    bool VkmAliasedMemoryHeap::validate(const std::vector<VkmAliasLifetime>& lifetimes, std::string* outError) const
    {
        uint32_t subGraphCount = 0;
        for (const VkmAliasLifetime& lifetime : lifetimes)
        {
            subGraphCount = std::max(subGraphCount, lifetime._last + 1);
        }

        for (const Block& block : _blocks)
        {
            for (size_t i = 0; i < block._occupants.size(); ++i)
            {
                for (size_t j = i + 1; j < block._occupants.size(); ++j)
                {
                    const Occupant& a = block._occupants[i];
                    const Occupant& b = block._occupants[j];
                    if (!rangesOverlap(a._offset, a._size, b._offset, b._size))
                    {
                        continue;
                    }
                    const VkmAliasLifetime lifetimeA = lifetimeOf(lifetimes, a._handle, subGraphCount);
                    const VkmAliasLifetime lifetimeB = lifetimeOf(lifetimes, b._handle, subGraphCount);
                    if (!lifetimeA.overlaps(lifetimeB))
                    {
                        continue;
                    }

                    if (outError != nullptr)
                    {
                        *outError = fmt::format(
                            "aliased resources {} [{},{}] and {} [{},{}] now overlap, but share bytes "
                            "[{},{}); placement was frozen when their lifetimes were disjoint",
                            a._handle.id, lifetimeA._first, lifetimeA._last,
                            b._handle.id, lifetimeB._first, lifetimeB._last,
                            std::max(a._offset, b._offset),
                            std::min(a._offset + a._size, b._offset + b._size));
                    }
                    return false;
                }
            }
        }
        return true;
    }

    std::optional<VkmAliasPlacement> VkmAliasedMemoryHeap::getPlacement(VkmResourceHandle handle) const
    {
        const Registration* registration = findRegistration(handle);
        return registration != nullptr ? registration->_placement : std::nullopt;
    }

    bool VkmAliasedMemoryHeap::isAliased(VkmResourceHandle handle) const
    {
        const Registration* registration = findRegistration(handle);
        if (registration == nullptr || !registration->_placement.has_value())
        {
            return false;
        }

        const VkmAliasPlacement& placement = *registration->_placement;
        const std::vector<Occupant>& occupants = _blocks[placement._blockIndex]._occupants;
        return std::any_of(occupants.begin(), occupants.end(), [&](const Occupant& occupant) {
            return occupant._handle != handle &&
                   rangesOverlap(placement._offset, placement._size, occupant._offset, occupant._size);
        });
    }

    uint64_t VkmAliasedMemoryHeap::getReservedBytes() const
    {
        uint64_t reserved = 0;
        for (const Block& block : _blocks)
        {
            reserved += block._size;
        }
        return reserved;
    }
} // namespace vkm
