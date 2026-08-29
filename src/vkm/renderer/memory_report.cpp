// Copyright (c) 2026 Snowapril

#include <vkm/renderer/memory_report.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <new>
#include <unordered_map>

// Symbolization runs in-process: dladdr names the symbol an address falls inside, and
// __cxa_demangle turns that mangled name into a readable one. Both are POSIX/Itanium-ABI, which
// Emscripten does not have and Windows does not either (it would need DbgHelp and a different
// resolver entirely). Both keep the raw addresses and everything below compiles out.
#if !defined(VKM_PLATFORM_WASM) && !defined(VKM_PLATFORM_WINDOWS)
#define VKM_MEMORY_REPORT_SYMBOLIZE
#endif

#if defined(VKM_MEMORY_REPORT_SYMBOLIZE)
#include <cxxabi.h>
#include <dlfcn.h>
#include <cstdlib>
#endif

namespace vkm
{
    namespace
    {
        std::string formatLine(const char* format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return std::string(buffer);
        }

        /*
        * @brief Process-wide cache of symbolized call sites. Code addresses never move, so an entry
        * is good for the life of the process.
        * @details Immortal for the same reason MemoryTracker is: its strings are freed through the
        * tracked operator delete and therefore through mimalloc, whose process-exit cleanup runs on
        * a schedule unrelated to C++ statics, so letting this map destruct at exit is a coin flip
        * on whether mimalloc is still alive to take the frees. The OS reclaims it either way.
        */
        struct CallSiteCache
        {
            std::mutex _mutex;
            std::unordered_map<const void*, std::string> _names;
        };

        CallSiteCache& callSiteCache()
        {
            alignas(CallSiteCache) static unsigned char storage[sizeof(CallSiteCache)];
            static CallSiteCache* instance = ::new (storage) CallSiteCache();
            return *instance;
        }

        // Returns nullptr when `address` has not been resolved yet.
        const std::string* findCachedCallSite(const void* address)
        {
            CallSiteCache& cache = callSiteCache();
            std::lock_guard<std::mutex> lock(cache._mutex);
            const auto it = cache._names.find(address);
            return (it == cache._names.end()) ? nullptr : &it->second;
        }

        std::string formatAddress(const void* address)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%llx",
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
            return std::string(buffer);
        }

#if defined(VKM_MEMORY_REPORT_SYMBOLIZE)
        /*
        * Trims one demangled symbol down to what a table cell can show: the argument list dwarfs
        * everything useful about a call site, so the signature is cut at its first '('.
        */
        std::string condenseSymbolName(const std::string& symbol)
        {
            std::string function = symbol.substr(0, std::min(symbol.find('('), symbol.size()));
            // libc++ decorates its symbols with an ABI tag ("[abi:de200100]") that says nothing
            // about where the allocation came from and costs a third of the column width.
            for (size_t tagStart = function.find("[abi:"); tagStart != std::string::npos;
                 tagStart = function.find("[abi:", tagStart))
            {
                const size_t tagEnd = function.find(']', tagStart);
                if (tagEnd == std::string::npos)
                {
                    break;
                }
                function.erase(tagStart, tagEnd - tagStart + 1);
            }
            while (!function.empty() && function.back() == ' ')
            {
                function.pop_back();
            }
            return function;
        }
#endif // VKM_MEMORY_REPORT_SYMBOLIZE
    } // namespace

    std::string formatByteSize(uint64_t bytes)
    {
        constexpr uint64_t kKiB = 1024ull;
        constexpr uint64_t kMiB = kKiB * 1024ull;
        constexpr uint64_t kGiB = kMiB * 1024ull;

        char buffer[32];
        if (bytes >= kGiB)
        {
            std::snprintf(buffer, sizeof(buffer), "%.2f GiB", static_cast<double>(bytes) / static_cast<double>(kGiB));
        }
        else if (bytes >= kMiB)
        {
            std::snprintf(buffer, sizeof(buffer), "%.1f MiB", static_cast<double>(bytes) / static_cast<double>(kMiB));
        }
        else if (bytes >= kKiB)
        {
            std::snprintf(buffer, sizeof(buffer), "%.1f KiB", static_cast<double>(bytes) / static_cast<double>(kKiB));
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
        }
        return std::string(buffer);
    }

    void resolveMemoryTagCallSites(const std::vector<TaggedAllocationSummary>& tags)
    {
#if !defined(VKM_MEMORY_REPORT_SYMBOLIZE)
        (void)tags;
#else
        for (const TaggedAllocationSummary& tag : tags)
        {
            if (tag.callSite == nullptr || findCachedCallSite(tag.callSite) != nullptr)
            {
                continue;
            }

            std::string name;
            Dl_info info{};
            if (::dladdr(tag.callSite, &info) != 0 && info.dli_sname != nullptr)
            {
                int status = 0;
                // __cxa_demangle allocates through malloc rather than the replaced global
                // operator new, so resolving cannot recurse back into MemoryTracker::allocate.
                char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                name = condenseSymbolName((status == 0 && demangled != nullptr) ? demangled
                                                                                : info.dli_sname);
                std::free(demangled);
            }

            // Cached either way. An address dladdr cannot name -- a static or hidden-visibility
            // function is absent from the dynamic symbol table -- caches empty, so it is
            // attempted exactly once rather than on every snapshot for the life of the process.
            CallSiteCache& cache = callSiteCache();
            std::lock_guard<std::mutex> lock(cache._mutex);
            cache._names.emplace(tag.callSite, std::move(name));
        }
#endif
    }

    std::string formatMemoryTagName(const TaggedAllocationSummary& tag)
    {
        if (tag.label != nullptr)
        {
            return std::string(tag.label);
        }
        if (tag.callSite != nullptr)
        {
            const std::string* resolved = findCachedCallSite(tag.callSite);
            // An empty entry is the negative cache: this address has no name to show.
            return (resolved != nullptr && !resolved->empty()) ? *resolved
                                                               : formatAddress(tag.callSite);
        }
        if (tag.file == nullptr)
        {
            return std::string("<unknown>");
        }

        // Call-site tags carry __FILE__, which is an absolute path in this build; only the
        // file name is useful in a table cell.
        const std::string path(tag.file);
        const size_t separator = path.find_last_of("/\\");
        const std::string fileName = (separator == std::string::npos) ? path : path.substr(separator + 1);
        return fileName + ":" + std::to_string(tag.line);
    }

    VkmMemorySnapshot captureMemorySnapshot(VkmDriverBase* driver, bool includeTagTable)
    {
        VkmMemorySnapshot snapshot;

        snapshot._process = getProcessMemoryStats();
        snapshot._mimalloc = MemoryTracker::singleton().getMimallocStats();

        if (includeTagTable)
        {
            std::vector<TaggedAllocationSummary> tags = MemoryTracker::singleton().getTaggedAllocations();
            snapshot._cpuTags.reserve(tags.size());
            for (const TaggedAllocationSummary& tag : tags)
            {
                // The tracker keeps a tag forever once seen; a fully-freed one is noise here.
                if (tag.liveCount == 0)
                {
                    continue;
                }
                snapshot._cpuTrackedRequestedBytes += tag.requestedBytes;
                snapshot._cpuTrackedUsableBytes += tag.usableBytes;
                snapshot._cpuTrackedLiveCount += tag.liveCount;
                snapshot._cpuTags.push_back(tag);
            }
            std::sort(snapshot._cpuTags.begin(), snapshot._cpuTags.end(),
                      [](const TaggedAllocationSummary& lhs, const TaggedAllocationSummary& rhs) {
                          return lhs.usableBytes > rhs.usableBytes;
                      });

            // Done here rather than at format time so a snapshot is self-describing for every
            // consumer -- inspector, shutdown dump, tests. Only call sites seen for the first
            // time cost anything.
            resolveMemoryTagCallSites(snapshot._cpuTags);
        }
        else
        {
            // The same three numbers the loop above sums, read straight off the tracker's
            // counters: a fully-freed tag carries zero bytes, so dropping those rows changes
            // nothing about the totals.
            const TrackedTotals totals = MemoryTracker::singleton().getTaggedTotals();
            snapshot._cpuTrackedRequestedBytes = totals.requestedBytes;
            snapshot._cpuTrackedUsableBytes = totals.usableBytes;
            snapshot._cpuTrackedLiveCount = totals.liveCount;
        }

        if (driver != nullptr)
        {
            snapshot._gpu = driver->getGpuMemoryStats();

            if (VkmRenderResourcePool* pool = driver->getRenderResourcePool())
            {
                snapshot._gpuTotal = pool->getAllCategoryMemoryUsage(&snapshot._gpuByCategory);
            }
        }

        return snapshot;
    }

    void logMemoryReport(const VkmMemorySnapshot& snapshot)
    {
        VKM_DEBUG_INFO("=== Memory report ===");

        if (snapshot._process._valid)
        {
            VKM_DEBUG_INFO(formatLine("Process: resident %s, peak %s, virtual %s",
                                      formatByteSize(snapshot._process._residentBytes).c_str(),
                                      formatByteSize(snapshot._process._peakResidentBytes).c_str(),
                                      formatByteSize(snapshot._process._virtualBytes).c_str()).c_str());
        }
        else
        {
            VKM_DEBUG_INFO("Process: unavailable on this platform");
        }

        VKM_DEBUG_INFO(formatLine("Allocator: committed %s, rss %s, page faults %zu",
                                  formatByteSize(snapshot._mimalloc.currentCommittedBytes).c_str(),
                                  formatByteSize(snapshot._mimalloc.currentRssBytes).c_str(),
                                  snapshot._mimalloc.pageFaults).c_str());

        VKM_DEBUG_INFO(formatLine("CPU tracked: %s usable (%s requested) across %llu live allocations",
                                  formatByteSize(snapshot._cpuTrackedUsableBytes).c_str(),
                                  formatByteSize(snapshot._cpuTrackedRequestedBytes).c_str(),
                                  static_cast<unsigned long long>(snapshot._cpuTrackedLiveCount)).c_str());

        // Top offenders only: a full tag dump at shutdown would bury everything else.
        constexpr size_t kLoggedTagCount = 10;
        const size_t loggedTags = std::min(kLoggedTagCount, snapshot._cpuTags.size());
        for (size_t i = 0; i < loggedTags; ++i)
        {
            const TaggedAllocationSummary& tag = snapshot._cpuTags[i];
            VKM_DEBUG_INFO(formatLine("  %-40s %6zu live  %10s usable",
                                      formatMemoryTagName(tag).c_str(), tag.liveCount,
                                      formatByteSize(tag.usableBytes).c_str()).c_str());
        }
        if (snapshot._cpuTags.size() > loggedTags)
        {
            VKM_DEBUG_INFO(formatLine("  ... and %zu more tags", snapshot._cpuTags.size() - loggedTags).c_str());
        }

        VKM_DEBUG_INFO(formatLine("GPU tracked: %s allocated (%s requested) across %u live resources",
                                  formatByteSize(snapshot._gpuTotal.totalAllocatedBytes).c_str(),
                                  formatByteSize(snapshot._gpuTotal.totalRequestedBytes).c_str(),
                                  snapshot._gpuTotal.liveCount).c_str());
        for (uint8_t type = 0; type < static_cast<uint8_t>(VkmResourceType::Count); ++type)
        {
            const VkmResourceCategoryUsage& usage = snapshot._gpuByCategory[type];
            if (usage.liveCount == 0)
            {
                continue;
            }
            VKM_DEBUG_INFO(formatLine("  %-40s %6u live  %10s allocated",
                                      vkmResourceTypeName(static_cast<VkmResourceType>(type)),
                                      usage.liveCount,
                                      formatByteSize(usage.totalAllocatedBytes).c_str()).c_str());
        }

        if (snapshot._gpu._hasDeviceStats)
        {
            VKM_DEBUG_INFO(formatLine("GPU device: %s allocated of %s budget (%s beyond what the engine tracked)",
                                      formatByteSize(snapshot._gpu._deviceAllocatedBytes).c_str(),
                                      formatByteSize(snapshot._gpu._deviceBudgetBytes).c_str(),
                                      formatByteSize(snapshot._gpu._deviceAllocatedBytes > snapshot._gpuTotal.totalAllocatedBytes
                                                         ? snapshot._gpu._deviceAllocatedBytes - snapshot._gpuTotal.totalAllocatedBytes
                                                         : 0).c_str()).c_str());
        }
        else
        {
            VKM_DEBUG_INFO("GPU device: not reported by this backend");
        }

        if (snapshot._gpu._hasPoolStats)
        {
            VKM_DEBUG_INFO(formatLine("GPU suballocator: %s used of %s reserved",
                                      formatByteSize(snapshot._gpu._poolUsedBytes).c_str(),
                                      formatByteSize(snapshot._gpu._poolReservedBytes).c_str()).c_str());
        }
    }
} // namespace vkm
