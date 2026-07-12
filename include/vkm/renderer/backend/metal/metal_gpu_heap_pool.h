// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

@protocol MTLHeap;
@protocol MTLBuffer;

namespace vkm
{
    class VkmDriverMetal;

    /*
    * @brief Wraps one MTLHeapTypeAutomatic heap used for the pooled placement path of
    * regular (non-staging) buffers. Unlike the Vulkan pool, freeing an individual buffer
    * needs no explicit release call -- ARC releasing the id<MTLBuffer> lets the heap
    * reclaim that space internally.
    */
    class VkmGpuHeapPoolMetal
    {
    public:
        static constexpr const uint64_t POOL_BLOCK_SIZE_BYTES = 64ull * 1024 * 1024;

        explicit VkmGpuHeapPoolMetal(VkmDriverMetal* driver);
        ~VkmGpuHeapPoolMetal();

        bool initialize();

        // Returns nil if there isn't enough space for sizeBytes at the given alignment.
        // `options` is an MTLResourceOptions bitmask, passed as a raw integer so this header
        // doesn't need to import Metal's resource-options header.
        id<MTLBuffer> tryAllocateBuffer(uint64_t sizeBytes, uint64_t alignment, uint64_t options);

    private:
        VkmDriverMetal* _driver;
        id<MTLHeap> _mtlHeap{nullptr};
    };
} // namespace vkm
