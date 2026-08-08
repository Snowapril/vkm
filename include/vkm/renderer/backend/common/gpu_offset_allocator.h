// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <memory>

namespace vkm
{
    struct VkmGpuMemoryAllocation
    {
        static constexpr const uint32_t INVALID_ALLOCATION_METADATA = 0xffffffff;

        uint32_t _offset = INVALID_ALLOCATION_METADATA;
        uint32_t _metadata = INVALID_ALLOCATION_METADATA; // opaque; pass back unchanged to free()

        inline bool isValid() const { return _metadata != INVALID_ALLOCATION_METADATA; }
    };

    /*
    * @brief CPU-side offset allocator packing many small sub-allocations into a single large
    * backing buffer, bound at draw/dispatch/copy time via offset+range.
    * @details Not used for texture or dedicated-memory placement, which VMA on Vulkan and MTLHeap
    * on Metal already provide.
    */
    class VkmOffsetAllocator
    {
    public:
        explicit VkmOffsetAllocator(uint32_t poolSizeBytes, uint32_t maxAllocs = 4096);
        ~VkmOffsetAllocator();

        VkmGpuMemoryAllocation allocate(uint32_t sizeBytes, uint32_t alignment);
        void free(const VkmGpuMemoryAllocation& allocation);

        inline uint32_t getPoolSizeBytes() const { return _poolSizeBytes; }

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
        uint32_t _poolSizeBytes;
    };
} // namespace vkm
