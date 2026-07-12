// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/platform.h>
#include <cstddef>
#include <mutex>
#include <memory_resource>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkm
{
    /*
    * @brief Snapshot of mimalloc's own process-wide statistics.
    * @details All fields are 0 on VKM_PLATFORM_WASM, where mimalloc is not used and
    * global new/delete fall back to the platform's default allocator.
    */
    struct MemoryStats
    {
        size_t currentCommittedBytes = 0;
        size_t peakCommittedBytes = 0;
        size_t currentRssBytes = 0;
        size_t peakRssBytes = 0;
        size_t pageFaults = 0;
    };

    /*
    * @brief Aggregated live-allocation counters for a single memory tag.
    * @details A tag is either an auto-captured call site (file/line set, label == nullptr)
    * or a user-supplied label (file == nullptr, line == 0, label set). "Untagged" is the
    * sentinel label used for allocations that did not go through VKM_NEW/VKM_NEW_TAGGED
    * (plain new/delete, make_unique, STL containers, third-party code).
    */
    struct TaggedAllocationSummary
    {
        const char* file = nullptr;
        int line = 0;
        const char* label = nullptr;
        size_t liveCount = 0;
        size_t requestedBytes = 0;
        size_t usableBytes = 0; // mimalloc's actual usable size for the live allocations; == requestedBytes on WASM
    };

    /*
    * @brief Central mimalloc-backed allocation tracker.
    * @details Every operator new/delete in the process routes through this singleton
    * (see the global operator new/delete overrides in src/vkm/base/memory.cpp).
    * VKM_NEW/VKM_NEW_TAGGED additionally attribute the allocation to a tag so it can be
    * queried via getTaggedAllocations().
    */
    class MemoryTracker
    {
    public:
        static MemoryTracker& singleton();

        MemoryTracker(const MemoryTracker&) = delete;
        MemoryTracker& operator=(const MemoryTracker&) = delete;

        /*
        * @brief Allocate `size` bytes tagged with the given call site or label.
        * @details Intended for use by VKM_NEW/VKM_NEW_TAGGED and the global operator
        * new/delete overrides - not meant to be called directly elsewhere. `label`, when
        * non-null, must be a string literal or otherwise have static storage duration:
        * it is stored by pointer, never copied.
        */
        void* allocate(size_t size, const char* file, int line, const char* label);

        /*
        * @brief Free a pointer previously returned by allocate().
        */
        void deallocate(void* taggedPtr) noexcept;

        /*
        * @brief Query mimalloc's own process-wide statistics.
        */
        MemoryStats getMimallocStats() const;

        /*
        * @brief Query per-tag live allocation counters (requested vs. mimalloc's actual
        * usable/committed size). Tags are never removed once seen, so a tag with
        * liveCount == 0 means it has no allocations live right now, not that it never had any.
        */
        std::vector<TaggedAllocationSummary> getTaggedAllocations() const;

    private:
        MemoryTracker();

        // Identifies an aggregate row: either (file, line) for an auto-captured call site,
        // or `label` alone for a manually-tagged/sentinel row (file/line ignored when
        // label != nullptr).
        struct TagKey
        {
            const char* file = nullptr;
            int line = 0;
            const char* label = nullptr;

            bool operator==(const TagKey& other) const noexcept;
        };

        struct TagKeyHash
        {
            size_t operator()(const TagKey& key) const noexcept;
        };

        struct TagStats
        {
            size_t liveCount = 0;
            size_t requestedBytes = 0;
            size_t usableBytes = 0;
        };

        // Backs _tagStats with raw mi_malloc/mi_free, bypassing the tracked global
        // operator new/delete entirely. Without this, inserting a new tag into _tagStats
        // would allocate map-node storage via the very operator new this class overrides,
        // which would call back into allocate() to track that node's own allocation,
        // which would insert into _tagStats again - infinite recursion.
        class RawMimallocResource : public std::pmr::memory_resource
        {
        protected:
            void* do_allocate(size_t bytes, size_t alignment) override;
            void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
            bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
        };

        mutable std::mutex _mutex;
        // mutable: do_allocate/do_deallocate don't mutate observable state (they just
        // forward to mi_malloc/mi_free), but getTaggedAllocations() const needs a
        // non-const memory_resource* to build its scratch pmr::vector.
        mutable RawMimallocResource _rawResource;
        std::pmr::unordered_map<TagKey, TagStats, TagKeyHash> _tagStats;
    };

    namespace detail
    {
        template <typename T, typename... Args>
        T* trackedNew(const char* file, int line, const char* label, Args&&... args)
        {
            void* mem = MemoryTracker::singleton().allocate(sizeof(T), file, line, label);
            return ::new (mem) T(std::forward<Args>(args)...);
        }

        // Compile-time enforcement that VKM_NEW_TAGGED's label is a string literal (or
        // other array with static storage duration): allocate() stores `label` by pointer
        // rather than copying it, so a non-literal argument here would be a dangling-pointer
        // trap. Deducing `const char (&)[N]` rejects plain `const char*` arguments (including
        // runtime-computed or short-lived ones) at the call site. Callers that legitimately
        // hold a label pointer with static storage duration but not literal-array type (e.g.
        // a `constexpr const char*` variable) should call detail::trackedNew directly - that
        // is the documented escape hatch around this check.
        template <typename T, size_t N, typename... Args>
        T* trackedNewTagged(const char* file, int line, const char (&label)[N], Args&&... args)
        {
            return trackedNew<T>(file, line, label, std::forward<Args>(args)...);
        }
    }

    // Tag a heap allocation with its own call site (file/line), captured automatically.
    // Deallocate with plain `delete ptr;` - there is no corresponding VKM_DELETE macro;
    // the tag is recovered from a header stored alongside the allocation, not the call site.
    #define VKM_NEW(Type, ...) \
        ::vkm::detail::trackedNew<Type>(__FILE__, __LINE__, nullptr, ##__VA_ARGS__)

    // Tag a heap allocation with a user-supplied label instead of file/line. `Label` must be
    // a string literal (enforced at compile time - see detail::trackedNewTagged).
    #define VKM_NEW_TAGGED(Type, Label, ...) \
        ::vkm::detail::trackedNewTagged<Type>(__FILE__, __LINE__, (Label), ##__VA_ARGS__)
}
