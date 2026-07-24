// Copyright (c) 2025 Snowapril

#include <vkm/renderer/memory_report.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

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

    std::string formatMemoryTagName(const TaggedAllocationSummary& tag)
    {
        if (tag.label != nullptr)
        {
            return std::string(tag.label);
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
