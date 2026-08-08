// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>

#include <memory>

@protocol MTLHeap;
@protocol MTLBuffer;
@protocol MTLTexture;
@class MTLTextureDescriptor;

namespace vkm
{
    class VkmDriverMetal;

    /*
    * @brief One MTLHeapTypePlacement heap carved into sub-ranges by VkmOffsetAllocator.
    * @details The Metal counterpart to VkmGpuBufferPoolVulkan, and it carries the same
    * obligation: a placement heap tracks nothing about what is placed in it, so every
    * allocation must be handed back with release() or the block leaks that range. Releasing
    * the id<MTLBuffer>/id<MTLTexture> alone does not reclaim it.
    *
    * Resources placed at overlapping offsets alias. Nothing may hand out an overlapping range,
    * which is the invariant VkmOffsetAllocator maintains; the heap requests
    * MTLHazardTrackingModeTracked so placed resources synchronize like any other resource.
    *
    * Buffers and textures share one block. Their footprints must come from
    * heapBufferSizeAndAlignWithLength: / heapTextureSizeAndAlignWithDescriptor:, which include
    * the padding Metal needs and are not derivable from a length or an extent.
    */
    class VkmGpuHeapPoolMetal
    {
    public:
        static constexpr const uint64_t POOL_BLOCK_SIZE_BYTES = 64ull * 1024 * 1024;

        explicit VkmGpuHeapPoolMetal(VkmDriverMetal* driver);
        ~VkmGpuHeapPoolMetal();

        bool initialize();

        /*
        * @brief Places a buffer at a newly reserved offset in this block.
        * @param lengthBytes Length the buffer reports; what the caller asked for.
        * @param footprintBytes Bytes reserved in the heap, from
        * heapBufferSizeAndAlignWithLength:options:. Never smaller than lengthBytes, and the
        * two are reserved separately so the buffer does not over-report its own length.
        * @param alignment Alignment from that same query.
        * @param options MTLResourceOptions bitmask, passed as a raw integer so this header
        * need not import Metal's resource-options header.
        * @param outAllocation Receives the reserved range; pass it back to release().
        * @return nil when the block has no room, leaving outAllocation invalid.
        */
        id<MTLBuffer> tryAllocateBuffer(uint64_t lengthBytes, uint64_t footprintBytes, uint64_t alignment,
                                        uint64_t options, VkmGpuMemoryAllocation* outAllocation);

        /*
        * @brief Places a texture at a newly reserved offset in this block.
        * @param descriptor Storage mode must match the heap's MTLStorageModePrivate.
        * @param footprintBytes Bytes reserved in the heap, from heapTextureSizeAndAlignWithDescriptor:.
        * @param alignment Alignment from that same query.
        * @param outAllocation Receives the reserved range; pass it back to release().
        * @return nil when the block has no room, leaving outAllocation invalid.
        */
        id<MTLTexture> tryAllocateTexture(MTLTextureDescriptor* descriptor, uint64_t footprintBytes,
                                          uint64_t alignment, VkmGpuMemoryAllocation* outAllocation);

        /*
        * @brief Returns a range to the block.
        * @details The Metal object placed there must already be released, or the next
        * placement at that offset aliases a live resource.
        * @param allocation Range handed out by tryAllocateBuffer/tryAllocateTexture.
        * @param footprintBytes The same footprint that was passed when it was reserved.
        */
        void release(const VkmGpuMemoryAllocation& allocation, uint64_t footprintBytes);

        inline id<MTLHeap> getHeap() const { return _mtlHeap; }

        // Bytes handed out and not yet released. Tracked here because MTLHeap.usedSize does
        // not account for what a placement heap holds.
        inline uint64_t getUsedBytes() const { return _usedBytes; }

    private:
        VkmDriverMetal* _driver;
        id<MTLHeap> _mtlHeap{nullptr};
        std::unique_ptr<VkmOffsetAllocator> _offsetAllocator;
        uint64_t _usedBytes{0};
    };
} // namespace vkm
