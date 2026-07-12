#include <doctest/doctest.h>

#include <vkm/base/memory.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>

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

TEST_CASE("MemoryTracker - plain new/delete still routes through mimalloc under the Untagged bucket") {
    auto before = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), "Untagged");
    const size_t liveBefore = before.has_value() ? before->liveCount : 0;

    int* value = new int(5);
    REQUIRE(value != nullptr);
    CHECK(*value == 5);

    auto during = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), "Untagged");
    REQUIRE(during.has_value());
    CHECK(during->liveCount >= liveBefore + 1);
    const size_t liveDuring = during->liveCount;

    delete value;

    auto after = findEntry(vkm::MemoryTracker::singleton().getTaggedAllocations(), "Untagged");
    REQUIRE(after.has_value());
    CHECK(after->liveCount <= liveDuring - 1);
}

#if !defined(VKM_PLATFORM_WASM)
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
