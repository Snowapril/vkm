// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief One aliasable texture's live range, as a closed interval of subgraph indices within
    * a single VkmRenderGraph.
    * @details Subgraphs commit in declaration order into one command buffer, so a lifetime is
    * an integer interval and "these two never coexist" is interval non-intersection -- no
    * dependency graph or topological sort is involved. Both ends are inclusive: a resource
    * touched only by subgraph 3 has _first == _last == 3.
    */
    struct VkmAliasLifetime
    {
        VkmResourceHandle _handle;
        uint32_t _first = 0;
        uint32_t _last = 0;

        inline bool overlaps(const VkmAliasLifetime& other) const
        {
            return !(_last < other._first || other._last < _first);
        }
    };

    /*
    * @brief Where an aliasable texture's bytes live, once chosen.
    * @details Assigned by the first VkmRenderGraph::compile() that declares the resource and
    * never revisited -- see VkmAliasedMemoryHeap::place().
    */
    struct VkmAliasPlacement
    {
        uint32_t _blockIndex = 0;
        uint64_t _offset = 0;
        uint64_t _size = 0;
        uint64_t _alignment = 1;
    };

    /*
    * @brief Decides which aliasable textures may share bytes, and where each one sits.
    *
    * @details Pure bookkeeping: it owns no device memory and makes no API calls. The driver
    * supplies blocks through VkmDriverBase::onCreateAliasBlock/onDestroyAliasBlock, and the
    * backends turn a VkmAliasPlacement into a real binding (Vulkan vmaBindImageMemory2 at an
    * offset, Metal -[MTLHeap newTextureWithDescriptor:offset:]). That split is what lets the
    * packing rules be unit-tested with no GPU at all.
    *
    * **Placement is final.** Vulkan can bind an image's memory only once and needs bound memory
    * before a view can be created, and VkmResourceTableBase bakes that view in at construction --
    * so moving a resource later would invalidate every table naming it. place() therefore only
    * ever assigns resources that have none yet; validate() is what later frames call to confirm
    * the lifetimes still justify the placement already in force.
    */
    class VkmAliasedMemoryHeap
    {
    public:
        // Matches VkmGpuBufferPoolVulkan and VkmGpuHeapPoolMetal, so all three pools reserve in
        // the same granularity and one number explains the memory report.
        static constexpr uint64_t BLOCK_SIZE_BYTES = 64ull * 1024 * 1024;

        explicit VkmAliasedMemoryHeap(VkmDriverBase* driver);
        ~VkmAliasedMemoryHeap();

        /*
        * @brief Declare a texture's memory requirements. Called from the backend's initialize()
        * once the native object exists but before it has memory.
        * @param memoryTypeBits Vulkan's VkMemoryRequirements::memoryTypeBits. Metal has no such
        * concept and passes ~0u, which makes every block compatible.
        */
        bool registerResource(VkmResourceHandle handle, uint64_t sizeBytes, uint64_t alignment,
                              uint32_t memoryTypeBits);

        // Drops the resource and frees its range for a later placement. Called from
        // VkmRenderResourcePool::releaseResource(), i.e. after the deferred reclaimer has
        // established the GPU is done with it.
        void unregisterResource(VkmResourceHandle handle);

        /*
        * @brief Assign a block and offset to every registered resource that has none.
        * @details Resources already placed keep their placement and act as fixed occupants, so
        * the result depends on registration history rather than on this call's contents alone.
        * Newly placed handles come back through outNewlyPlaced so the caller can finalize each
        * one's native binding.
        */
        bool place(const std::vector<VkmAliasLifetime>& lifetimes, std::vector<VkmResourceHandle>* outNewlyPlaced);

        /*
        * @brief Confirm the placements already in force are still justified by these lifetimes.
        * @details The check every frame after the first runs. Returns false and describes the
        * first conflict when two resources sharing bytes have come to overlap -- which means the
        * graph's shape changed after placement was frozen.
        */
        bool validate(const std::vector<VkmAliasLifetime>& lifetimes, std::string* outError) const;

        std::optional<VkmAliasPlacement> getPlacement(VkmResourceHandle handle) const;

        // True when this resource's bytes are shared with at least one other placed resource, so
        // it needs an acquisition barrier before its first use. A lone aliasable texture does not.
        bool isAliased(VkmResourceHandle handle) const;

        // Bytes reserved in blocks -- what the memory report should attribute to aliasing, since
        // each aliased texture itself reports zero to avoid double-counting.
        uint64_t getReservedBytes() const;
        uint32_t getBlockCount() const { return (uint32_t)_blocks.size(); }

        void destroy();

    private:
        struct Occupant
        {
            VkmResourceHandle _handle;
            uint64_t _offset = 0;
            uint64_t _size = 0;
        };

        struct Block
        {
            uint64_t _size = 0;
            // Fixed by the first placement; a later resource joins only if its memoryTypeBits
            // admit this block, otherwise a new one opens.
            uint32_t _memoryTypeBits = ~0u;
            std::vector<Occupant> _occupants;
        };

        struct Registration
        {
            VkmResourceHandle _handle;
            uint64_t _size = 0;
            uint64_t _alignment = 1;
            uint32_t _memoryTypeBits = ~0u;
            std::optional<VkmAliasPlacement> _placement;
        };

        Registration* findRegistration(VkmResourceHandle handle);
        const Registration* findRegistration(VkmResourceHandle handle) const;
        // Lifetime of a registered resource that this frame did not declare: live for the whole
        // graph. Treating it as empty would let two never-declared resources look mutually
        // disjoint, which is false the moment either is used in a later frame.
        static VkmAliasLifetime lifetimeOf(const std::vector<VkmAliasLifetime>& lifetimes,
                                           VkmResourceHandle handle, uint32_t subGraphCount);
        bool tryPlaceInBlock(Block& block, uint32_t blockIndex, const Registration& registration,
                             const VkmAliasLifetime& lifetime, const std::vector<VkmAliasLifetime>& lifetimes,
                             uint32_t subGraphCount, VkmAliasPlacement* outPlacement) const;

        VkmDriverBase* _driver;
        std::vector<Block> _blocks;
        std::vector<Registration> _registrations;
    };
} // namespace vkm
