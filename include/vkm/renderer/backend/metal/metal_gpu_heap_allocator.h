// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>

#include <memory>
#include <vector>

@protocol MTLBuffer;
@protocol MTLTexture;
@class MTLTextureDescriptor;

namespace vkm
{
    class VkmDriverMetal;
    class VkmGpuHeapPoolMetal;
    struct VkmGpuMemoryStats;

    /*
    * @brief Owns the MTLHeapTypePlacement blocks backing VkmMemoryPlacementHint::Heap resources.
    * @details Buffers and textures share the same blocks. A request larger than one block is
    * refused rather than served, so callers must treat a nil return as "allocate this committed
    * instead" rather than as an error.
    *
    * Every successful allocation hands back a Placement that must be returned with release()
    * once the Metal object is gone -- a placement heap reclaims nothing on its own, so dropping
    * the Placement leaks that range for the life of the block.
    */
    class VkmGpuHeapAllocatorMetal
    {
    public:
        /*
        * @brief A reserved range inside one heap block.
        * @details Opaque to the resource holding it; hand it back to release() unchanged.
        * ownerBlock is non-owning and stays valid until the allocator is destroyed.
        */
        struct Placement
        {
            VkmGpuMemoryAllocation range{};
            VkmGpuHeapPoolMetal* ownerBlock{nullptr};
            uint64_t footprintBytes{0};

            inline bool isValid() const { return ownerBlock != nullptr && range.isValid(); }
        };

        explicit VkmGpuHeapAllocatorMetal(VkmDriverMetal* driver);
        ~VkmGpuHeapAllocatorMetal();

        /*
        * @brief Places a buffer in a heap block, growing the list by one block if needed.
        * @param lengthBytes Length the buffer reports; what the caller asked for.
        * @param footprintBytes Bytes reserved in the heap, from
        * heapBufferSizeAndAlignWithLength:options:.
        * @param alignment Alignment from that same query.
        * @param options MTLResourceOptions bitmask, passed as a raw integer so this header
        * need not import Metal's resource-options header.
        * @param outPlacement Receives the reserved range; pass it back to release().
        * @return nil when the request cannot be served; the caller falls back to committed.
        */
        id<MTLBuffer> allocateBuffer(uint64_t lengthBytes, uint64_t footprintBytes, uint64_t alignment,
                                     uint64_t options, Placement* outPlacement);

        /*
        * @brief Places a texture in a heap block, growing the list by one block if needed.
        * @param descriptor Storage mode must be MTLStorageModePrivate to match the heap's.
        * @param footprintBytes Bytes reserved in the heap, from
        * heapTextureSizeAndAlignWithDescriptor:; padded for tiling, not derivable from the extent.
        * @param alignment Alignment from that same query.
        * @param outPlacement Receives the reserved range; pass it back to release().
        * @return nil when the request cannot be served; the caller falls back to committed.
        */
        id<MTLTexture> allocateTexture(MTLTextureDescriptor* descriptor, uint64_t footprintBytes,
                                       uint64_t alignment, Placement* outPlacement);

        // Returns a range to its block. The Metal object placed there must already be released.
        void release(const Placement& placement);

        // Adds this allocator's reserved/used bytes to the driver-level memory report.
        void accumulateMemoryStats(VkmGpuMemoryStats* outStats) const;

        // Releases every block. Must run after the resources placed in them are gone.
        void destroy();

    private:
        VkmGpuHeapPoolMetal* growBlock();

        VkmDriverMetal* _driver;
        std::vector<std::unique_ptr<VkmGpuHeapPoolMetal>> _blocks;
    };
} // namespace vkm
