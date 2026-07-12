// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/gpu_offset_allocator.h>

#include <offsetAllocator.hpp>

namespace vkm
{
    struct VkmOffsetAllocator::Impl
    {
        OffsetAllocator::Allocator allocator;

        Impl(uint32_t poolSizeBytes, uint32_t maxAllocs)
            : allocator(poolSizeBytes, maxAllocs)
        {
        }
    };

    VkmOffsetAllocator::VkmOffsetAllocator(uint32_t poolSizeBytes, uint32_t maxAllocs)
        : _impl(std::make_unique<Impl>(poolSizeBytes, maxAllocs))
        , _poolSizeBytes(poolSizeBytes)
    {
    }

    VkmOffsetAllocator::~VkmOffsetAllocator() = default;

    VkmGpuMemoryAllocation VkmOffsetAllocator::allocate(uint32_t sizeBytes, uint32_t alignment)
    {
        // OffsetAllocator has no native alignment parameter: over-request enough slack to
        // round the returned offset up to `alignment`, then discard the padding. The opaque
        // `metadata` (internal node index) is preserved unchanged and is all `free()` needs --
        // it never reads `offset` back, so the alignment padding is always safe to discard.
        const uint32_t paddedSize = sizeBytes + (alignment > 1 ? (alignment - 1) : 0);
        const OffsetAllocator::Allocation rawAllocation = _impl->allocator.allocate(paddedSize);

        VkmGpuMemoryAllocation allocation{};
        if (rawAllocation.offset == OffsetAllocator::Allocation::NO_SPACE)
        {
            return allocation; // isValid() == false
        }

        const uint32_t alignedOffset = (alignment > 1)
            ? ((rawAllocation.offset + alignment - 1) / alignment) * alignment
            : rawAllocation.offset;

        allocation._offset = alignedOffset;
        allocation._metadata = rawAllocation.metadata;
        return allocation;
    }

    void VkmOffsetAllocator::free(const VkmGpuMemoryAllocation& allocation)
    {
        if (!allocation.isValid())
        {
            return;
        }

        OffsetAllocator::Allocation rawAllocation{};
        rawAllocation.offset = allocation._offset;
        rawAllocation.metadata = static_cast<OffsetAllocator::NodeIndex>(allocation._metadata);
        _impl->allocator.free(rawAllocation);
    }
} // namespace vkm
