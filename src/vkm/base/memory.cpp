// Copyright (c) 2025 Snowapril

#include <vkm/base/memory.h>

#include <cstdlib>
#include <new>

#if !defined(VKM_PLATFORM_WASM)
#include <mimalloc.h>
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
        size_t requestedSize;
        size_t usableSize;
    };
    static_assert(sizeof(AllocationHeader) % alignof(std::max_align_t) == 0,
                  "AllocationHeader must preserve max_align_t alignment for the user pointer that follows it");

    constexpr const char* kUntaggedLabel = "Untagged";
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
        return file == other.file && line == other.line;
    }

    size_t MemoryTracker::TagKeyHash::operator()(const TagKey& key) const noexcept
    {
        if (key.label != nullptr)
        {
            return std::hash<std::string_view>{}(std::string_view(key.label));
        }
        return std::hash<const void*>{}(key.file) ^ (std::hash<int>{}(key.line) << 1);
    }

    void* MemoryTracker::RawMimallocResource::do_allocate(size_t bytes, size_t /*alignment*/)
    {
#if !defined(VKM_PLATFORM_WASM)
        return mi_malloc(bytes);
#else
        return std::malloc(bytes);
#endif
    }

    void MemoryTracker::RawMimallocResource::do_deallocate(void* ptr, size_t /*bytes*/, size_t /*alignment*/)
    {
#if !defined(VKM_PLATFORM_WASM)
        mi_free(ptr);
#else
        std::free(ptr);
#endif
    }

    bool MemoryTracker::RawMimallocResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
    {
        return this == &other;
    }

    MemoryTracker::MemoryTracker()
        : _tagStats(&_rawResource)
    {
    }

    MemoryTracker::~MemoryTracker() = default;

    MemoryTracker& MemoryTracker::singleton()
    {
        // Intentionally never destroyed. mimalloc registers its own process-exit cleanup
        // (mi_process_done, via __attribute__((destructor)) - see mimalloc's prim/prim.c)
        // completely independently of the normal atexit/__cxa_atexit machinery that runs
        // C++ static object destructors. There is no portable guarantee about the relative
        // order of the two: if this tracker's own destructor ran and freed its bookkeeping
        // map's nodes via mi_free after mimalloc's own shutdown had already run (or vice
        // versa), that's a use-after-shutdown crash. Since the OS reclaims all process
        // memory on exit regardless, there is nothing to gain from ever tearing this down -
        // placement-new into static storage with no matching destructor call sidesteps the
        // ordering question entirely.
        //
        // Construction itself must never call operator new (directly or via a heap-
        // allocated member) - that would recursively re-enter this function while it's
        // still being initialized, which is undefined behavior. Placement new doesn't call
        // operator new, and _tagStats/_rawResource are direct (non-heap) members, so
        // construction here never does.
        alignas(MemoryTracker) static unsigned char storage[sizeof(MemoryTracker)];
        static MemoryTracker* instance = ::new (storage) MemoryTracker();
        return *instance;
    }

    void* MemoryTracker::allocate(size_t size, const char* file, int line, const char* label)
    {
        const TagKey key = label != nullptr ? TagKey{ nullptr, 0, label } : TagKey{ file, line, nullptr };
        const size_t totalSize = sizeof(AllocationHeader) + size;

#if !defined(VKM_PLATFORM_WASM)
        void* raw = mi_malloc(totalSize);
        const size_t usable = mi_usable_size(raw) - sizeof(AllocationHeader);
#else
        void* raw = std::malloc(totalSize);
        const size_t usable = size;
#endif

        auto* header = static_cast<AllocationHeader*>(raw);
        header->file = key.file;
        header->line = key.line;
        header->label = key.label;
        header->requestedSize = size;
        header->usableSize = usable;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            TagStats& stats = _tagStats[key];
            stats.liveCount += 1;
            stats.requestedBytes += size;
            stats.usableBytes += usable;
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
        const TagKey key = header->label != nullptr
            ? TagKey{ nullptr, 0, header->label }
            : TagKey{ header->file, header->line, nullptr };

        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _tagStats.find(key);
            if (it != _tagStats.end())
            {
                it->second.liveCount -= 1;
                it->second.requestedBytes -= header->requestedSize;
                it->second.usableBytes -= header->usableSize;
            }
        }

#if !defined(VKM_PLATFORM_WASM)
        mi_free(header);
#else
        std::free(header);
#endif
    }

    MemoryStats MemoryTracker::getMimallocStats() const
    {
        MemoryStats stats{};
#if !defined(VKM_PLATFORM_WASM)
        size_t elapsedMsecs = 0, userMsecs = 0, systemMsecs = 0;
        size_t currentRss = 0, peakRss = 0, currentCommit = 0, peakCommit = 0, pageFaults = 0;
        mi_process_info(&elapsedMsecs, &userMsecs, &systemMsecs,
                         &currentRss, &peakRss, &currentCommit, &peakCommit, &pageFaults);
        stats.currentCommittedBytes = currentCommit;
        stats.peakCommittedBytes = peakCommit;
        stats.currentRssBytes = currentRss;
        stats.peakRssBytes = peakRss;
        stats.pageFaults = pageFaults;
#endif
        return stats;
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
                    key.file, key.line, key.label,
                    stats.liveCount, stats.requestedBytes, stats.usableBytes });
            }
        }
        return std::vector<TaggedAllocationSummary>(scratch.begin(), scratch.end());
    }
}

// Global operator new/delete overrides: every allocation in the process routes through
// vkm::MemoryTracker (and therefore mimalloc), tagged "Untagged" unless it went through
// VKM_NEW/VKM_NEW_TAGGED. operator new(std::nothrow_t) is intentionally not overridden -
// the standard-mandated default nothrow wrapper already calls the (replaceable) throwing
// operator new(size_t) in a try/catch, so it automatically routes through the override
// below with no functional loss, and nothing in this codebase uses new(std::nothrow).
void* operator new(std::size_t size)
{
    return vkm::MemoryTracker::singleton().allocate(size, nullptr, 0, kUntaggedLabel);
}

void* operator new[](std::size_t size)
{
    return vkm::MemoryTracker::singleton().allocate(size, nullptr, 0, kUntaggedLabel);
}

void operator delete(void* ptr) noexcept
{
    vkm::MemoryTracker::singleton().deallocate(ptr);
}

void operator delete[](void* ptr) noexcept
{
    vkm::MemoryTracker::singleton().deallocate(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    vkm::MemoryTracker::singleton().deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    vkm::MemoryTracker::singleton().deallocate(ptr);
}
