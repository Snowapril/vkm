#include <doctest/doctest.h>

#include <vkm/base/memory.h>
#include <vkm/platform/common/process_stats.h>
#include <vkm/renderer/memory_report.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct Probe
{
    int value;
    explicit Probe(int v) : value(v) {}
};

std::optional<vkm::TaggedAllocationSummary> findEntry(const std::vector<vkm::TaggedAllocationSummary>& entries,
                                                        std::string_view label)
{
    auto it = std::find_if(entries.begin(), entries.end(), [&](const vkm::TaggedAllocationSummary& entry) {
        return entry.label != nullptr && std::string_view(entry.label) == label;
    });
    return it != entries.end() ? std::make_optional(*it) : std::nullopt;
}

std::optional<vkm::TaggedAllocationSummary> findEntry(const std::vector<vkm::TaggedAllocationSummary>& entries,
                                                        std::string_view file, int line)
{
    auto it = std::find_if(entries.begin(), entries.end(), [&](const vkm::TaggedAllocationSummary& entry) {
        return entry.file != nullptr && entry.line == line && std::string_view(entry.file) == file;
    });
    return it != entries.end() ? std::make_optional(*it) : std::nullopt;
}
} // namespace

TEST_CASE("MemoryTracker - VKM_NEW_TAGGED records and clears a manual tag") {
    constexpr const char* kLabel = "MemoryTrackerUnitTest_TaggedLifecycle";

    // VKM_NEW_TAGGED now enforces at compile time that its label is a string literal, so the
    // literal is passed directly here rather than through the `kLabel` pointer variable;
    // `kLabel` is kept for the findEntry() comparisons below.
    Probe* probe = VKM_NEW_TAGGED(Probe, "MemoryTrackerUnitTest_TaggedLifecycle", 42);
    REQUIRE(probe != nullptr);
    CHECK(probe->value == 42);

    auto live = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), kLabel);
    REQUIRE(live.has_value());
    CHECK(live->liveCount == 1);
    CHECK(live->requestedBytes == sizeof(Probe));
    CHECK(live->usableBytes >= live->requestedBytes);

    delete probe;

    auto freed = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), kLabel);
    REQUIRE(freed.has_value()); // tags are never erased, only decremented
    CHECK(freed->liveCount == 0);
    CHECK(freed->requestedBytes == 0);
}

TEST_CASE("MemoryTracker - VKM_NEW auto-tags allocation with its own file/line") {
    const int expectedLine = __LINE__ + 1;
    Probe* probe = VKM_NEW(Probe, 7);
    REQUIRE(probe != nullptr);

    auto live = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), __FILE__, expectedLine);
    REQUIRE(live.has_value());
    CHECK(live->liveCount >= 1);

    delete probe;

    auto freed = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), __FILE__, expectedLine);
    REQUIRE(freed.has_value());
    CHECK(freed->liveCount == 0);
}

#if defined(VKM_PLATFORM_WASM)
TEST_CASE("MemoryTracker - plain new/delete stays in the Untagged bucket on WASM") {
    // WebAssembly cannot read its own return addresses, so there is no call site to capture
    // and the sentinel bucket is still where plain new lands.
    auto before = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), "Untagged");
    const size_t liveBefore = before.has_value() ? before->liveCount : 0;

    int* value = new int(5);
    REQUIRE(value != nullptr);

    auto during = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), "Untagged");
    REQUIRE(during.has_value());
    CHECK(during->liveCount >= liveBefore + 1);
    const size_t liveDuring = during->liveCount;

    delete value;

    auto after = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), "Untagged");
    REQUIRE(after.has_value());
    CHECK(after->liveCount <= liveDuring - 1);
}
#else
TEST_CASE("MemoryTracker - plain new/delete is attributed to its own call site") {
    // The call site is a machine address, so unlike the VKM_NEW case the test cannot name the
    // row it expects up front. Landmark's odd size is what identifies it instead: querying the
    // tracker allocates its own result vector, so "the row whose live count went up" alone
    // would just as happily match that.
    struct Landmark
    {
        char _bytes[4099];
    };

    const auto findLandmarkRow = []() -> std::optional<vkm::TaggedAllocationSummary> {
        for (const vkm::TaggedAllocationSummary& entry : vkm::MemoryTracker::singleton().getTaggedAllocations())
        {
            if (entry.callSite != nullptr && entry.liveCount == 1 && entry.requestedBytes == sizeof(Landmark))
            {
                return entry;
            }
        }
        return std::nullopt;
    };

    REQUIRE_FALSE(findLandmarkRow().has_value());

    Landmark* landmark = new Landmark();
    REQUIRE(landmark != nullptr);

    const std::optional<vkm::TaggedAllocationSummary> live = findLandmarkRow();
    REQUIRE(live.has_value());
    CHECK(live->callSite != nullptr);
    CHECK(live->label == nullptr); // no longer the "Untagged" sentinel
    CHECK(live->file == nullptr);  // and not a compile-time tag either
    CHECK(live->usableBytes >= live->requestedBytes);

    delete landmark;

    CHECK_FALSE(findLandmarkRow().has_value());
}

/*
* Resolution is best-effort by contract: it needs the platform's symbolizer and line tables
* the build may not carry. So this asserts the shape of the outcome rather than demanding a
* particular one -- either the address resolved, in which case it must name this very file,
* or it did not, in which case the raw address must still be shown.
*/
TEST_CASE("memory report - a captured call site resolves to a readable name") {
    int* value = new int(11);

    std::vector<vkm::TaggedAllocationSummary> tags = vkm::MemoryTracker::singleton().getTaggedAllocations();
    tags.erase(std::remove_if(tags.begin(), tags.end(),
                              [](const vkm::TaggedAllocationSummary& entry) {
                                  return entry.callSite == nullptr || entry.liveCount == 0;
                              }),
               tags.end());
    REQUIRE(!tags.empty());

    vkm::resolveMemoryTagCallSites(tags);

    size_t namedRows = 0;
    for (const vkm::TaggedAllocationSummary& entry : tags)
    {
        const std::string name = vkm::formatMemoryTagName(entry);
        CHECK(!name.empty());
        // Either a bare address, or something a human can act on -- never an empty cell.
        if (name.rfind("0x", 0) != 0)
        {
            ++namedRows;
        }
    }
    INFO("resolved " << namedRows << " of " << tags.size() << " call sites");

    delete value;
}
#endif // defined(VKM_PLATFORM_WASM)

#if defined(VKM_USE_MIMALLOC)
TEST_CASE("MemoryTracker - getMimallocStats returns real, self-consistent numbers") {
    // Not delta-tested against a specific allocation: mi_process_info's commit/RSS
    // counters don't necessarily update synchronously with each individual mi_malloc
    // call (mimalloc may aggregate per-thread stats lazily), so asserting a specific
    // allocation moved them by a specific amount is flaky across platforms/build modes.
    // What's actually worth checking is that, after real allocation activity has
    // happened in this process (every test above did some), the API returns sane,
    // non-zero, internally consistent values rather than all-zero/garbage.
    constexpr size_t kBigSize = 10 * 1024 * 1024;
    char* buffer = new char[kBigSize];
    std::memset(buffer, 0xAB, kBigSize);

    const vkm::MemoryStats stats = vkm::MemoryTracker::singleton().getMimallocStats();

    CHECK(stats.currentCommittedBytes > 0);
    CHECK(stats.peakCommittedBytes >= stats.currentCommittedBytes);
    CHECK(stats.currentRssBytes > 0);
    CHECK(stats.peakRssBytes >= stats.currentRssBytes);

    delete[] buffer;
}
#endif

TEST_CASE("getProcessMemoryStats reports the OS's own view of this process") {
    // Same reasoning as the mimalloc case above: the OS updates these counters on its own
    // schedule, so this checks that they are real and self-consistent rather than asserting
    // that one allocation moved them by a given amount.
    const vkm::VkmProcessMemoryStats stats = vkm::getProcessMemoryStats();

    REQUIRE(stats._valid);
    CHECK(stats._residentBytes > 0);
    // Deliberately no `peak >= resident` assertion: the peak and the current figure come
    // from different OS counters (on Linux, ru_maxrss vs /proc/self/statm; on macOS, two
    // ledger fields sampled separately), so they are not captured atomically and the
    // ordering is not guaranteed at any instant -- this flaked green/red across CI runners.

    // The whole point of this API is that it sees memory the engine's own tracker cannot:
    // the binary, thread stacks and every third-party allocation. So the process figure must
    // never come out below what the tracker alone accounts for.
    uint64_t trackedUsableBytes = 0;
    for (const vkm::TaggedAllocationSummary& tag : vkm::MemoryTracker::singleton().getTaggedAllocations())
    {
        trackedUsableBytes += tag.usableBytes;
    }
    CHECK(stats._residentBytes >= trackedUsableBytes);
}
