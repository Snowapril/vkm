// Copyright (c) 2025 Snowapril

#include <vkm/renderer/memory_report.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <new>
#include <unordered_map>

// Symbolization is a host-tooling job: it needs to know which module an address belongs to
// and read that module's debug info. Emscripten has neither, so the WASM build keeps the raw
// addresses and everything below compiles out.
#if !defined(VKM_PLATFORM_WASM)
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
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
        * @brief Process-wide cache of symbolized call sites; code addresses never move, so an
        * entry is good for the life of the process.
        *
        * Immortal for the same reason MemoryTracker is (see its singleton()): its strings are
        * freed through the tracked operator delete, and therefore through mimalloc, whose own
        * process-exit cleanup runs on a destructor schedule unrelated to C++ statics. Letting
        * this map destruct at exit is a coin flip on whether mimalloc is still alive to take
        * the frees -- a crash after the last line of main. Nothing is lost by never tearing it
        * down; the OS reclaims it either way.
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

#if !defined(VKM_PLATFORM_WASM)
        // Runs `argv` and returns its stdout. posix_spawn rather than popen: this process is
        // multithreaded and replaces global operator new, and fork() in that situation can
        // deadlock in the child if anything between fork and exec touches the allocator.
        // posix_spawn has no such window.
        std::string captureCommandOutput(char* const argv[])
        {
            int pipeFds[2] = { -1, -1 };
            if (::pipe(pipeFds) != 0)
            {
                return std::string();
            }

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
            posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

            pid_t pid = -1;
            const int spawnResult = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv, environ);
            posix_spawn_file_actions_destroy(&actions);
            ::close(pipeFds[1]);

            if (spawnResult != 0)
            {
                ::close(pipeFds[0]);
                return std::string();
            }

            std::string output;
            char buffer[4096];
            ssize_t readBytes = 0;
            while ((readBytes = ::read(pipeFds[0], buffer, sizeof(buffer))) > 0)
            {
                output.append(buffer, static_cast<size_t>(readBytes));
            }
            ::close(pipeFds[0]);

            int status = 0;
            ::waitpid(pid, &status, 0);
            return output;
        }

        // One module's worth of addresses to symbolize, in the order their results come back.
        struct ModuleBatch
        {
            const char* _path = nullptr;
            const void* _loadAddress = nullptr;
            std::vector<const void*> _addresses;
        };

        /*
        * Trims one symbolizer output line down to what a table cell can show.
        *
        * atos prints "func(args) (in module) (file.cpp:123)" and addr2line -f -C prints the
        * function and the "file.cpp:123" on separate lines, already joined by the caller. In
        * both cases the argument list dwarfs everything useful, so the signature is cut at its
        * first '(' while the trailing source location -- the last parenthesised group -- is
        * kept. A line with no source info degrades to just the function name.
        */
        std::string condenseSymbolLine(const std::string& line)
        {
            if (line.empty())
            {
                return std::string();
            }

            std::string source;
            const size_t sourceOpen = line.find_last_of('(');
            if (sourceOpen != std::string::npos && line.back() == ')')
            {
                const std::string candidate = line.substr(sourceOpen + 1, line.size() - sourceOpen - 2);
                // "(in module)" is atos naming the binary, not a source location.
                if (candidate.rfind("in ", 0) != 0 && candidate.find(':') != std::string::npos)
                {
                    source = candidate;
                }
            }

            std::string function = line.substr(0, std::min(line.find('('), line.size()));
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
            if (function.empty())
            {
                return source;
            }
            return source.empty() ? function : (function + " (" + source + ")");
        }

        // Symbolizes one module's addresses and writes whatever it recovers into the cache.
        void resolveModuleBatch(const ModuleBatch& batch)
        {
            // Bounded so the argument list cannot approach ARG_MAX on a snapshot with
            // thousands of distinct call sites.
            constexpr size_t kAddressesPerInvocation = 128;

            for (size_t first = 0; first < batch._addresses.size(); first += kAddressesPerInvocation)
            {
                const size_t last = std::min(first + kAddressesPerInvocation, batch._addresses.size());

                std::vector<std::string> storage;
                std::vector<char*> argv;
#if defined(VKM_PLATFORM_APPLE)
                // atos is the only thing on macOS that reads the linker's debug map, which is
                // where a non-dSYM Debug build keeps its line tables.
                char loadAddress[32];
                std::snprintf(loadAddress, sizeof(loadAddress), "0x%llx",
                              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(batch._loadAddress)));
                storage = { "atos", "-o", batch._path, "-l", loadAddress };
#else
                storage = { "addr2line", "-f", "-C", "-e", batch._path };
#endif
                for (size_t i = first; i < last; ++i)
                {
                    // addr2line wants module-relative addresses; atos wants absolute ones and
                    // is told the load address separately.
#if defined(VKM_PLATFORM_APPLE)
                    storage.push_back(formatAddress(batch._addresses[i]));
#else
                    const uintptr_t offset = reinterpret_cast<uintptr_t>(batch._addresses[i]) -
                                             reinterpret_cast<uintptr_t>(batch._loadAddress);
                    storage.push_back(formatAddress(reinterpret_cast<const void*>(offset)));
#endif
                }

                argv.reserve(storage.size() + 1);
                for (std::string& argument : storage)
                {
                    argv.push_back(argument.data());
                }
                argv.push_back(nullptr);

                const std::string output = captureCommandOutput(argv.data());
                if (output.empty())
                {
                    continue;
                }

                // One result line per address on Apple; addr2line -f emits two (function, then
                // source), so they are folded back into one before parsing.
                std::vector<std::string> lines;
                for (size_t begin = 0; begin < output.size();)
                {
                    const size_t end = std::min(output.find('\n', begin), output.size());
                    lines.push_back(output.substr(begin, end - begin));
                    begin = end + 1;
                }
#if !defined(VKM_PLATFORM_APPLE)
                std::vector<std::string> folded;
                for (size_t i = 0; i + 1 < lines.size(); i += 2)
                {
                    folded.push_back(lines[i] + " (" + lines[i + 1] + ")");
                }
                lines.swap(folded);
#endif

                CallSiteCache& cache = callSiteCache();
                std::lock_guard<std::mutex> lock(cache._mutex);
                for (size_t i = first; i < last && (i - first) < lines.size(); ++i)
                {
                    std::string name = condenseSymbolLine(lines[i - first]);
                    if (!name.empty())
                    {
                        cache._names[batch._addresses[i]] = std::move(name);
                    }
                }
            }
        }
#endif // !defined(VKM_PLATFORM_WASM)
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
#if defined(VKM_PLATFORM_WASM)
        (void)tags;
#else
        // Grouped by module: each one costs a child process, and every address in a shared
        // library has to be symbolized against that library rather than the executable.
        std::map<const void*, ModuleBatch> batches;
        for (const TaggedAllocationSummary& tag : tags)
        {
            if (tag.callSite == nullptr || findCachedCallSite(tag.callSite) != nullptr)
            {
                continue;
            }

            Dl_info info{};
            if (::dladdr(tag.callSite, &info) == 0 || info.dli_fname == nullptr || info.dli_fbase == nullptr)
            {
                continue;
            }

            ModuleBatch& batch = batches[info.dli_fbase];
            batch._path = info.dli_fname;
            batch._loadAddress = info.dli_fbase;
            batch._addresses.push_back(tag.callSite);
        }

        for (const auto& [loadAddress, batch] : batches)
        {
            (void)loadAddress;
            resolveModuleBatch(batch);
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
            return (resolved != nullptr) ? *resolved : formatAddress(tag.callSite);
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

    VkmMemorySnapshot captureMemorySnapshot(VkmDriverBase* driver)
    {
        VkmMemorySnapshot snapshot;

        snapshot._process = getProcessMemoryStats();
        snapshot._mimalloc = MemoryTracker::singleton().getMimallocStats();

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
        // consumer -- inspector, shutdown dump, tests -- and so the cost lands on the sampling
        // path that is already documented as low-rate. Only call sites seen for the first time
        // cost anything.
        resolveMemoryTagCallSites(snapshot._cpuTags);

        if (driver != nullptr)
        {
            snapshot._gpu = driver->getGpuMemoryStats();

            if (VkmRenderResourcePool* pool = driver->getRenderResourcePool())
            {
                snapshot._gpuTotal = pool->getTotalMemoryUsage();
                for (uint8_t type = 0; type < static_cast<uint8_t>(VkmResourceType::Count); ++type)
                {
                    snapshot._gpuByCategory[type] = pool->getCategoryMemoryUsage(static_cast<VkmResourceType>(type));
                }
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
