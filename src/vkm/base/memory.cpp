// Copyright (c) 2026 Snowapril

#include <vkm/base/memory.h>

#include <cstdlib>
#include <new>

#if defined(VKM_USE_MIMALLOC)
#include <mimalloc.h>
#endif

// The address of the instruction that called the enclosing function. Both spellings are
// compiler intrinsics that compile to a single load, which is what keeps call-site capture
// affordable on the global allocation path.
//
// Deliberately absent on WASM: WebAssembly's call stack is not addressable from within the
// module, and there would be nothing to symbolize an address against even if it were. That
// platform keeps the "Untagged" bucket it always had.
#if defined(VKM_PLATFORM_WASM)
#define VKM_RETURN_ADDRESS() nullptr
#elif defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define VKM_RETURN_ADDRESS() _ReturnAddress()
#elif defined(__GNUC__) || defined(__clang__)
#define VKM_RETURN_ADDRESS() __builtin_return_address(0)
#else
#define VKM_RETURN_ADDRESS() nullptr
#endif

namespace
{
    // Header prepended to every tracked allocation; recovered via pointer arithmetic on
    // deallocate() so no per-pointer lookup table is needed (a per-pointer
    // std::unordered_map would recurse infinitely, since inserting into it would call
    // the very operator new this file overrides). Padded to alignof(std::max_align_t) so
    // the user pointer immediately following it keeps mimalloc's default alignment
    // guarantee; the codebase has no over-aligned types today (no `alignas`, no
    // forced-SIMD GLM config), so std::align_val_t overloads are intentionally not
    // handled here.
    struct alignas(alignof(std::max_align_t)) AllocationHeader
    {
        const char* file;
        int line;
        const char* label;
        const void* callSite;
        size_t requestedSize;
        size_t usableSize;
    };
    static_assert(sizeof(AllocationHeader) % alignof(std::max_align_t) == 0,
                  "AllocationHeader must preserve max_align_t alignment for the user pointer that follows it");

    constexpr const char* kUntaggedLabel = "Untagged";

    // Raw (untracked) allocation primitives, isolating the mimalloc-vs-WASM branching to a
    // single place. Used both by RawMimallocResource (the scratch allocator backing
    // MemoryTracker's own bookkeeping map) and by allocate()/deallocate() (the tracked
    // global operator new/delete path).
    void* rawAlloc(size_t bytes)
    {
#if defined(VKM_USE_MIMALLOC)
        return mi_malloc(bytes);
#else
        return std::malloc(bytes);
#endif
    }

    void rawFree(void* ptr)
    {
#if defined(VKM_USE_MIMALLOC)
        mi_free(ptr);
#else
        std::free(ptr);
#endif
    }

    // Number of bytes usable by the caller beyond `headerSize` for a block at `raw` whose
    // caller-requested size (excluding the header) is `requestedSize`. On non-WASM platforms
    // this reflects mimalloc's actual usable size for the whole block, which is always >=
    // requested since mimalloc rounds up to size-class boundaries. WASM has no mimalloc
    // equivalent query, so this returns `requestedSize` unchanged - usableBytes ==
    // requestedBytes is the intentional semantics on WASM documented on
    // TaggedAllocationSummary::usableBytes in memory.h.
    size_t usableSize(void* raw, size_t headerSize, size_t requestedSize)
    {
#if defined(VKM_USE_MIMALLOC)
        (void)requestedSize;
        return mi_usable_size(raw) - headerSize;
#else
        (void)raw;
        (void)headerSize;
        return requestedSize;
#endif
    }

    // Queries mimalloc's own process-wide statistics; returns a zeroed MemoryStats on WASM,
    // where mimalloc is not used (see MemoryStats's doc comment in memory.h). Passes nullptr
    // for mi_process_info's elapsed/user/system-time outputs since MemoryStats doesn't
    // surface them.
    vkm::MemoryStats rawProcessStats()
    {
        vkm::MemoryStats stats{};
#if defined(VKM_USE_MIMALLOC)
        mi_process_info(nullptr, nullptr, nullptr,
                         &stats.currentRssBytes, &stats.peakRssBytes,
                         &stats.currentCommittedBytes, &stats.peakCommittedBytes,
                         &stats.pageFaults);
#endif
        return stats;
    }
}

namespace vkm
{
    bool MemoryTracker::TagKey::operator==(const TagKey& other) const noexcept
    {
        if (label != nullptr || other.label != nullptr)
        {
            return label != nullptr && other.label != nullptr &&
                   std::string_view(label) == std::string_view(other.label);
        }
        // Raw address comparison: two call sites are the same row exactly when they are the
        // same instruction, which is also what makes this cheap enough for the hot path.
        if (callSite != nullptr || other.callSite != nullptr)
        {
            return callSite == other.callSite;
        }
        return file == other.file && line == other.line;
    }

    size_t MemoryTracker::TagKeyHash::operator()(const TagKey& key) const noexcept
    {
        if (key.label != nullptr)
        {
            return std::hash<std::string_view>{}(std::string_view(key.label));
        }
        if (key.callSite != nullptr)
        {
            return std::hash<const void*>{}(key.callSite);
        }
        return std::hash<const void*>{}(key.file) ^ (std::hash<int>{}(key.line) << 1);
    }

    void* MemoryTracker::RawMimallocResource::do_allocate(size_t bytes, size_t /*alignment*/)
    {
        return rawAlloc(bytes);
    }

    void MemoryTracker::RawMimallocResource::do_deallocate(void* ptr, size_t /*bytes*/, size_t /*alignment*/)
    {
        rawFree(ptr);
    }

    bool MemoryTracker::RawMimallocResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
    {
        return this == &other;
    }

    MemoryTracker::MemoryTracker()
        : _tagStats(&_rawResource)
    {
    }

    MemoryTracker& MemoryTracker::singleton()
    {
        // Intentionally never destroyed. mimalloc registers its own process-exit cleanup
        // (mi_process_done, via __attribute__((destructor))) independently of the atexit machinery
        // that runs C++ static destructors, and nothing orders the two. This tracker's destructor
        // freeing its map's nodes via mi_free after mimalloc's shutdown, or the reverse, is a
        // use-after-shutdown crash. The OS reclaims process memory on exit regardless, so
        // placement-new into static storage with no matching destructor sidesteps the ordering.
        //
        // Construction must never call operator new, directly or through a heap-allocated member:
        // that would re-enter this function while it is still initializing. Placement new does not
        // call operator new, and _tagStats/_rawResource are direct members.
        alignas(MemoryTracker) static unsigned char storage[sizeof(MemoryTracker)];
        static MemoryTracker* instance = ::new (storage) MemoryTracker();
        return *instance;
    }

    void* MemoryTracker::allocate(size_t size, const char* file, int line, const char* label,
                                  const void* callSite)
    {
        const TagKey key = label != nullptr      ? TagKey{ nullptr, 0, label, nullptr }
                           : callSite != nullptr ? TagKey{ nullptr, 0, nullptr, callSite }
                                                 : TagKey{ file, line, nullptr, nullptr };
        const size_t totalSize = sizeof(AllocationHeader) + size;

        void* raw = rawAlloc(totalSize);
        if (raw == nullptr)
        {
            throw std::bad_alloc();
        }
        const size_t usable = usableSize(raw, sizeof(AllocationHeader), size);

        auto* header = static_cast<AllocationHeader*>(raw);
        header->file = key.file;
        header->line = key.line;
        header->label = key.label;
        header->callSite = key.callSite;
        header->requestedSize = size;
        header->usableSize = usable;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            TagStats& stats = _tagStats[key];
            stats.liveCount += 1;
            stats.requestedBytes += size;
            stats.usableBytes += usable;
            _totals.liveCount += 1;
            _totals.requestedBytes += size;
            _totals.usableBytes += usable;
        }

        return static_cast<char*>(raw) + sizeof(AllocationHeader);
    }

    void MemoryTracker::deallocate(void* taggedPtr) noexcept
    {
        if (taggedPtr == nullptr)
        {
            return;
        }

        auto* header = reinterpret_cast<AllocationHeader*>(static_cast<char*>(taggedPtr) - sizeof(AllocationHeader));
        const TagKey key = header->label != nullptr      ? TagKey{ nullptr, 0, header->label, nullptr }
                           : header->callSite != nullptr ? TagKey{ nullptr, 0, nullptr, header->callSite }
                                                         : TagKey{ header->file, header->line, nullptr, nullptr };

        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _tagStats.find(key);
            if (it != _tagStats.end())
            {
                it->second.liveCount -= 1;
                it->second.requestedBytes -= header->requestedSize;
                it->second.usableBytes -= header->usableSize;
                _totals.liveCount -= 1;
                _totals.requestedBytes -= header->requestedSize;
                _totals.usableBytes -= header->usableSize;
            }
        }

        rawFree(header);
    }

    MemoryStats MemoryTracker::getMimallocStats() const
    {
        return rawProcessStats();
    }

    TrackedTotals MemoryTracker::getTaggedTotals() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _totals;
    }

    std::vector<TaggedAllocationSummary> MemoryTracker::getTaggedAllocations() const
    {
        // Snapshot into a pmr::vector backed by the same raw (untracked) resource while
        // holding _mutex, so growing this scratch buffer can't re-enter allocate() and
        // deadlock on the same non-recursive mutex. The final copy into a plain
        // std::vector happens after the lock is released.
        std::pmr::vector<TaggedAllocationSummary> scratch(&_rawResource);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            scratch.reserve(_tagStats.size());
            for (const auto& [key, stats] : _tagStats)
            {
                scratch.push_back(TaggedAllocationSummary{
                    key.file, key.line, key.label, key.callSite,
                    stats.liveCount, stats.requestedBytes, stats.usableBytes });
            }
        }
        return std::vector<TaggedAllocationSummary>(scratch.begin(), scratch.end());
    }
}

namespace
{
    void* trackedGlobalNew(std::size_t size, const void* callSite)
    {
        // A null call site can only happen where VKM_RETURN_ADDRESS is unavailable; the
        // "Untagged" bucket still exists for exactly that case.
        return callSite != nullptr
            ? vkm::MemoryTracker::singleton().allocate(size, nullptr, 0, nullptr, callSite)
            : vkm::MemoryTracker::singleton().allocate(size, nullptr, 0, kUntaggedLabel);
    }

    void trackedGlobalDelete(void* ptr) noexcept
    {
        vkm::MemoryTracker::singleton().deallocate(ptr);
    }
}

/*
* Global operator new/delete overrides: every allocation in the process routes through
* vkm::MemoryTracker, and therefore mimalloc. Allocations that went through VKM_NEW or
* VKM_NEW_TAGGED carry their own tag; everything else -- plain new, make_unique, STL containers,
* third-party code -- is attributed to the machine address of whoever called operator new, which
* the memory report symbolizes on demand.
*
* The return address is captured here rather than inside trackedGlobalNew because these are
* replaceable global functions no caller can inline away, so frame 0 is always the real allocation
* site. trackedGlobalNew is a static the compiler may inline, which would shift what frame 0 means.
*
* operator new(std::nothrow_t) is not overridden: the standard-mandated default nothrow wrapper
* calls the replaceable throwing operator new(size_t) in a try/catch, so it routes through the
* override below anyway.
*/
void* operator new(std::size_t size)
{
    return trackedGlobalNew(size, VKM_RETURN_ADDRESS());
}

void* operator new[](std::size_t size)
{
    return trackedGlobalNew(size, VKM_RETURN_ADDRESS());
}

void operator delete(void* ptr) noexcept
{
    trackedGlobalDelete(ptr);
}

void operator delete[](void* ptr) noexcept
{
    trackedGlobalDelete(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    trackedGlobalDelete(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    trackedGlobalDelete(ptr);
}
