// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

@protocol MTLHeap;
@protocol MTLBuffer;
@protocol MTLTexture;
@class MTLTextureDescriptor;

namespace vkm
{
    class VkmDriverMetal;

    /*
    * @brief Wraps one MTLHeapTypeAutomatic heap used for the Heap placement path of regular
    * (non-staging) buffers and textures. Unlike the Vulkan pool, freeing an individual
    * resource needs no explicit release call -- ARC releasing the id<MTLBuffer>/id<MTLTexture>
    * lets the heap reclaim that space internally.
    *
    * Buffers and textures share the same blocks: an MTLHeapTypeAutomatic heap holds both
    * interchangeably, so a second parallel list would add bookkeeping without buying anything.
    */
    class VkmGpuHeapPoolMetal
    {
    public:
        static constexpr const uint64_t POOL_BLOCK_SIZE_BYTES = 64ull * 1024 * 1024;

        explicit VkmGpuHeapPoolMetal(VkmDriverMetal* driver);
        ~VkmGpuHeapPoolMetal();

        bool initialize();

        // Whether this block can still fit sizeBytes at the given alignment.
        bool hasSpaceFor(uint64_t sizeBytes, uint64_t alignment) const;

        // Returns nil if there isn't enough space for sizeBytes at the given alignment.
        // `options` is an MTLResourceOptions bitmask, passed as a raw integer so this header
        // doesn't need to import Metal's resource-options header.
        id<MTLBuffer> tryAllocateBuffer(uint64_t sizeBytes, uint64_t alignment, uint64_t options);

        // Same contract for textures. `sizeBytes`/`alignment` must come from
        // heapTextureSizeAndAlignWithDescriptor: -- a texture's heap footprint is padded for
        // tiling and is not derivable from its extent the way a buffer's length is.
        id<MTLTexture> tryAllocateTexture(MTLTextureDescriptor* descriptor, uint64_t sizeBytes, uint64_t alignment);

        inline id<MTLHeap> getHeap() const { return _mtlHeap; }

    private:
        VkmDriverMetal* _driver;
        id<MTLHeap> _mtlHeap{nullptr};
    };
} // namespace vkm
