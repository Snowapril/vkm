// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/metal/metal_sparse_tile_heap.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/metal/metal_util.h>

#include <algorithm>

namespace vkm
{
    namespace
    {
        /*
        * Tiles one block holds. 4096 at the 16 KiB page size is 64 MiB, matching every other heap
        * block in the engine, and comfortably larger than the biggest run a material texture asks
        * for: level 0 of a 2048-wide chain is 32x32 = 1024 tiles. A level needing more than this
        * gets a block sized to it instead, since a run may not straddle blocks.
        */
        constexpr uint32_t kTilesPerBlock = 4096;
    } // namespace

    VkmSparseTileHeapMetal::VkmSparseTileHeapMetal(VkmDriverMetal* driver)
        : _driver(driver)
    {
    }

    VkmSparseTileHeapMetal::~VkmSparseTileHeapMetal()
    {
        destroy();
    }

    bool VkmSparseTileHeapMetal::initialize()
    {
        id<MTLDevice> device = _driver->getMTLDevice();
        _tileSizeBytes = [device sparseTileSizeInBytesForSparsePageSize:kVkmMetalSparsePageSize];
        if (_tileSizeBytes == 0)
        {
            VKM_DEBUG_ERROR("Sparse tile size came back zero; the tile heap cannot be sized");
            return false;
        }
        _tilesPerBlock = kTilesPerBlock;
        // Deliberately empty: a scene with no sparse texture should cost no heap at all, so the
        // first block waits for the first allocate().
        return true;
    }

    void VkmSparseTileHeapMetal::destroy()
    {
        VkmRenderResourcePoolMetal* pool =
            (_driver != nullptr) ? static_cast<VkmRenderResourcePoolMetal*>(_driver->getRenderResourcePool()) : nullptr;
        for (std::unique_ptr<Block>& block : _blocks)
        {
            if (block->_heap != nullptr)
            {
                if (pool != nullptr)
                {
                    pool->unregisterExternalAllocation(block->_heap);
                }
                [block->_heap release];
                block->_heap = nullptr;
            }
        }
        _blocks.clear();
        _allocatedTiles = 0;
    }

    bool VkmSparseTileHeapMetal::addBlock(uint32_t minimumTiles)
    {
        // A run may not straddle two blocks, because one mapping operation names one heap. So the
        // block, not the level, is what gives: a level 0 larger than the usual block gets a block
        // sized to it rather than being refused backing it could plainly have.
        const uint32_t blockTiles = std::max(_tilesPerBlock, minimumTiles);

        MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.size = _tileSizeBytes * blockTiles;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.type = MTLHeapTypePlacement;
        // Without this the heap cannot back a texture created at that page size at all; it is the
        // half of the agreement the texture's placementSparsePageSize is the other half of.
        descriptor.maxCompatiblePlacementSparsePageSize = kVkmMetalSparsePageSize;

        id<MTLHeap> heap = [_driver->getMTLDevice() newHeapWithDescriptor:descriptor];
        [descriptor release];
        if (heap == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create a sparse tile heap block");
            return false;
        }
        [heap setLabel:@"VkmSparseTileHeap"];

        // Every texture mapped from this heap reads through it, and Metal makes nothing resident
        // implicitly -- the same registration every other heap block in the engine does.
        static_cast<VkmRenderResourcePoolMetal*>(_driver->getRenderResourcePool())
            ->registerExternalAllocation(heap);

        auto block = std::make_unique<Block>();
        block->_heap = heap;
        block->_tileCount = blockTiles;
        block->_freeRuns.emplace_back(0u, blockTiles);
        _blocks.push_back(std::move(block));
        return true;
    }

    VkmSparseTileHeapMetal::TileRun VkmSparseTileHeapMetal::allocate(uint32_t tileCount)
    {
        if (tileCount == 0)
        {
            return TileRun{};
        }
        for (uint32_t attempt = 0; attempt < 2; ++attempt)
        {
            for (size_t blockIndex = 0; blockIndex < _blocks.size(); ++blockIndex)
            {
                Block& block = *_blocks[blockIndex];
                for (size_t runIndex = 0; runIndex < block._freeRuns.size(); ++runIndex)
                {
                    std::pair<uint32_t, uint32_t>& run = block._freeRuns[runIndex];
                    if (run.second < tileCount)
                    {
                        continue;
                    }

                    TileRun result{};
                    result._heap = block._heap;
                    result._blockIndex = static_cast<uint32_t>(blockIndex);
                    result._firstTile = run.first;
                    result._tileCount = tileCount;

                    // First fit, taking from the front so the remainder stays one run.
                    run.first += tileCount;
                    run.second -= tileCount;
                    if (run.second == 0)
                    {
                        block._freeRuns.erase(block._freeRuns.begin() + static_cast<long>(runIndex));
                    }
                    _allocatedTiles += tileCount;
                    return result;
                }
            }

            // Nothing had room; one more block, then try again exactly once.
            if (attempt == 0 && !addBlock(tileCount))
            {
                break;
            }
        }
        return TileRun{};
    }

    void VkmSparseTileHeapMetal::release(const TileRun& run)
    {
        if (!run.isValid() || run._blockIndex >= _blocks.size())
        {
            return;
        }
        Block& block = *_blocks[run._blockIndex];
        _allocatedTiles -= std::min(_allocatedTiles, run._tileCount);

        /*
        * Inserted in order and coalesced with either neighbour. Without the coalesce the free list
        * fragments into single tiles as levels stream in and out, and a level needing a contiguous
        * run would then fail against a heap that is mostly empty.
        */
        const auto insertAt =
            std::lower_bound(block._freeRuns.begin(), block._freeRuns.end(), run._firstTile,
                             [](const std::pair<uint32_t, uint32_t>& entry, uint32_t first) {
                                 return entry.first < first;
                             });
        const auto inserted = block._freeRuns.insert(insertAt, { run._firstTile, run._tileCount });

        auto merged = inserted;
        const auto next = merged + 1;
        if (next != block._freeRuns.end() && merged->first + merged->second == next->first)
        {
            merged->second += next->second;
            block._freeRuns.erase(next);
        }
        if (merged != block._freeRuns.begin())
        {
            const auto previous = merged - 1;
            if (previous->first + previous->second == merged->first)
            {
                previous->second += merged->second;
                block._freeRuns.erase(merged);
            }
        }
    }

    uint64_t VkmSparseTileHeapMetal::getAllocatedBytes() const
    {
        return static_cast<uint64_t>(_allocatedTiles) * _tileSizeBytes;
    }

    uint64_t VkmSparseTileHeapMetal::getReservedBytes() const
    {
        uint64_t tiles = 0;
        for (const std::unique_ptr<Block>& block : _blocks)
        {
            tiles += block->_tileCount;
        }
        return tiles * _tileSizeBytes;
    }
} // namespace vkm
