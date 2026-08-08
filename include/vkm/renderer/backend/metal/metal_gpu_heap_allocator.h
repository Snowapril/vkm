// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

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
    * @brief Owns the MTLHeap blocks backing VkmMemoryPlacementHint::Heap resources.
    * @details Buffers and textures share the same blocks -- an MTLHeapTypeAutomatic heap holds
    * both interchangeably. A request larger than one block is refused rather than served, so
    * callers must treat a nil return as "allocate this committed instead" rather than as an
    * error. Placed resources need no explicit free: releasing the id<MTLBuffer>/id<MTLTexture>
    * lets the heap reclaim that space.
    */
    class VkmGpuHeapAllocatorMetal
    {
    public:
        explicit VkmGpuHeapAllocatorMetal(VkmDriverMetal* driver);
        ~VkmGpuHeapAllocatorMetal();

        /*
        * @brief Places a buffer in a heap block, growing the list by one block if needed.
        * @param sizeBytes Length in bytes.
        * @param alignment Alignment from heapBufferSizeAndAlignWithLength:options:.
        * @param options MTLResourceOptions bitmask, passed as a raw integer so this header
        * need not import Metal's resource-options header.
        * @return nil when the request cannot be served; the caller falls back to committed.
        */
        id<MTLBuffer> allocateBuffer(uint64_t sizeBytes, uint64_t alignment, uint64_t options);

        /*
        * @brief Places a texture in a heap block, growing the list by one block if needed.
        * @param descriptor Storage mode must be MTLStorageModePrivate to match the heap's.
        * @param sizeBytes Heap footprint from heapTextureSizeAndAlignWithDescriptor:, which is
        * padded for tiling and is not derivable from the extent.
        * @param alignment Alignment from that same query.
        * @return nil when the request cannot be served; the caller falls back to committed.
        */
        id<MTLTexture> allocateTexture(MTLTextureDescriptor* descriptor, uint64_t sizeBytes, uint64_t alignment);

        // Adds this allocator's reserved/used bytes to the driver-level memory report.
        void accumulateMemoryStats(VkmGpuMemoryStats* outStats) const;

        // Releases every block. Must run before the driver drops its MTLDevice.
        void destroy();

    private:
        VkmGpuHeapPoolMetal* acquireBlockWithSpace(uint64_t sizeBytes, uint64_t alignment);

        VkmDriverMetal* _driver;
        std::vector<std::unique_ptr<VkmGpuHeapPoolMetal>> _blocks;
    };
} // namespace vkm
