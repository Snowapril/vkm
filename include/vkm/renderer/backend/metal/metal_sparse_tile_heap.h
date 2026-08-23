// Copyright (c) 2026 Snowapril

#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace vkm
{
    class VkmDriverMetal;

    /*
    * @brief The pool of fixed-size tiles that back the resident mip levels of every sparse texture.
    *
    * @details A placement-sparse texture owns no memory of its own: its levels have addresses, and
    * tiles from a heap are mapped onto them. This is that heap, plus the allocator deciding which
    * tiles go where.
    *
    * Runs, not single tiles. One mapping operation covers a rectangular tile region of one mip
    * level and draws it from a *contiguous* span of the heap starting at one tile offset, so the
    * unit of allocation is a run of N tiles rather than N separate tiles. That is why this is a
    * first-fit free list over tile indices and not a bitmap or an offset allocator: runs have to
    * stay contiguous, and the sizes are powers of four (a level is tilesX * tilesY), which packs
    * neatly and fragments little.
    *
    * Blocks grow on demand, like VkmGpuHeapAllocatorMetal's do. A run never straddles two blocks --
    * one mapping names one heap -- so a request larger than a block fails rather than being split,
    * and the block size is chosen to hold the largest level any material texture will ask for.
    *
    * Hazard tracking is left at a placement heap's default of untracked, for the same reason the
    * aliasing heap leaves it: tracked would serialise every resource in the heap against every
    * other. Ordering for a mapping change comes from an explicit MTLStageResourceState barrier.
    */
    class VkmSparseTileHeapMetal
    {
    public:
        explicit VkmSparseTileHeapMetal(VkmDriverMetal* driver);
        ~VkmSparseTileHeapMetal();

        VkmSparseTileHeapMetal(const VkmSparseTileHeapMetal&) = delete;
        VkmSparseTileHeapMetal& operator=(const VkmSparseTileHeapMetal&) = delete;

        /*
        * @brief One contiguous run of tiles within one heap block.
        * @details `_heap` is nil for an unallocated run, which is what a failed allocate returns.
        */
        struct TileRun
        {
            id<MTLHeap> _heap{nullptr};
            uint32_t _blockIndex = 0;
            uint32_t _firstTile = 0;
            uint32_t _tileCount = 0;

            inline bool isValid() const { return _heap != nullptr && _tileCount > 0; }
        };

        // Bytes one tile occupies, which is the sparse page size the device reports for it.
        bool initialize();
        void destroy();

        /*
        * @brief Takes a contiguous run of `tileCount` tiles, growing the pool if no block has room.
        * @param tileCount Tiles needed; a level's tilesX * tilesY.
        * @return The run, or an invalid one when the request exceeds a whole block or a new block
        * could not be created.
        */
        TileRun allocate(uint32_t tileCount);
        void release(const TileRun& run);

        inline uint64_t getTileSizeBytes() const { return _tileSizeBytes; }
        // Bytes committed across every block: what sparse residency actually costs right now, and
        // the number that moves as levels come and go -- a sparse texture's own allocatedSize
        // reports page-table footprint and never does.
        uint64_t getAllocatedBytes() const;
        uint64_t getReservedBytes() const;

    private:
        // One heap plus its free list. Kept in a vector whose entries are never reordered, so a
        // TileRun's block index stays valid for the pool's life.
        struct Block
        {
            id<MTLHeap> _heap{nullptr};
            uint32_t _tileCount = 0;
            // Free runs, kept sorted by first tile so neighbours can be coalesced on release.
            std::vector<std::pair<uint32_t, uint32_t>> _freeRuns;
        };

        // Adds one block, sized to hold at least `minimumTiles` in a single contiguous run.
        bool addBlock(uint32_t minimumTiles);

        VkmDriverMetal* _driver = nullptr;
        std::vector<std::unique_ptr<Block>> _blocks;
        uint64_t _tileSizeBytes = 0;
        uint32_t _tilesPerBlock = 0;
        uint32_t _allocatedTiles = 0;
    };
} // namespace vkm
