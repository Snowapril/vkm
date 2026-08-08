#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/aliased_memory_heap.h>
#include <vkm/renderer/backend/common/driver.h>

#include <algorithm>

/*
* Covers the packing rules that decide which aliasable textures may share bytes. No GPU and no
* real driver are involved: VkmAliasedMemoryHeap owns the decision, and the backends only turn a
* VkmAliasPlacement into a binding, which is exactly why the rules are testable here.
*
* The two properties that matter are that overlapping lifetimes never share bytes (getting that
* wrong corrupts pixels) and that placement is deterministic (the three per-frame-slot render
* graphs compile independently and must agree).
*/

namespace
{
    // Records the blocks the heap asks for; nothing else about a driver is exercised.
    class FakeAliasDriver : public vkm::VkmDriverBase
    {
    public:
        std::vector<uint64_t> _blockSizes;
        uint32_t _destroyedBlocks = 0;

        bool supportsResourceAliasing() const override { return true; }
        bool onCreateAliasBlock(uint32_t blockIndex, uint64_t sizeBytes, uint32_t memoryTypeBits) override
        {
            (void)blockIndex; (void)memoryTypeBits;
            _blockSizes.push_back(sizeBytes);
            return true;
        }
        void onDestroyAliasBlock(uint32_t) override { ++_destroyedBlocks; }

    protected:
        vkm::VkmInitResult initializeInner(const vkm::VkmEngineLaunchOptions*) override
        {
            return vkm::VkmInitResult{vkm::VkmInitResultCode::Success, ""};
        }
        void destroyInner() override {}
        vkm::VkmTexture* newTextureInner() override { return nullptr; }
        vkm::VkmBuffer* newBufferInner() override { return nullptr; }
        vkm::VkmStagingBuffer* newStagingBufferInner() override { return nullptr; }
        vkm::VkmSampler* newSamplerInner() override { return nullptr; }
        vkm::VkmTextureView* newTextureViewInner() override { return nullptr; }
        vkm::VkmBufferView* newBufferViewInner() override { return nullptr; }
        vkm::VkmSwapChainBase* newSwapChainInner() override { return nullptr; }
        vkm::VkmResourceTableBase* newResourceTableInner() override { return nullptr; }
        vkm::VkmAccelerationStructure* newAccelerationStructureInner() override { return nullptr; }
        vkm::VkmCommandQueueBase* newCommandQueueInner() override { return nullptr; }
        vkm::VkmPipelineStateBase* newPipelineStateInner() override { return nullptr; }
        vkm::VkmRenderResourcePool* newRenderResourcePoolInner() override { return nullptr; }
        vkm::VkmFormat selectSwapChainColorFormat(bool) const override { return vkm::VkmFormat::BGRA8_UNORM; }
    };

    vkm::VkmResourceHandle makeHandle(uint64_t id)
    {
        return vkm::VkmResourceHandle{id, vkm::VkmResourcePoolType::Aliased, vkm::VkmResourceType::Texture};
    }

    constexpr uint64_t kOneMiB = 1024ull * 1024;

    uint64_t offsetOf(const vkm::VkmAliasedMemoryHeap& heap, vkm::VkmResourceHandle handle)
    {
        const std::optional<vkm::VkmAliasPlacement> placement = heap.getPlacement(handle);
        REQUIRE(placement.has_value());
        return placement->_offset;
    }
}

TEST_CASE("VkmAliasedMemoryHeap - resources with disjoint lifetimes share the same bytes") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    // The case from the feature's own description: S1(R1 write) -> S2(R1 read) -> S3(R2 write)
    // -> S4(R2 read). R1 is dead before R2 is born, so one set of bytes serves both.
    const vkm::VkmResourceHandle r1 = makeHandle(1);
    const vkm::VkmResourceHandle r2 = makeHandle(2);
    REQUIRE(heap.registerResource(r1, kOneMiB, 256, ~0u));
    REQUIRE(heap.registerResource(r2, kOneMiB, 256, ~0u));

    std::vector<vkm::VkmResourceHandle> placed;
    REQUIRE(heap.place({{r1, 0, 1}, {r2, 2, 3}}, &placed));

    CHECK(placed.size() == 2);
    CHECK(offsetOf(heap, r1) == offsetOf(heap, r2));
    CHECK(heap.getBlockCount() == 1);
    CHECK(heap.isAliased(r1));
    CHECK(heap.isAliased(r2));
}

TEST_CASE("VkmAliasedMemoryHeap - overlapping lifetimes never share bytes") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    const vkm::VkmResourceHandle a = makeHandle(1);
    const vkm::VkmResourceHandle b = makeHandle(2);
    REQUIRE(heap.registerResource(a, kOneMiB, 256, ~0u));
    REQUIRE(heap.registerResource(b, kOneMiB, 256, ~0u));

    // The shape the gi sample actually has: the composite pass reads the direct target while
    // writing its own, so the two touch at subgraph 2 and cannot be aliased.
    REQUIRE(heap.place({{a, 0, 2}, {b, 2, 3}}, nullptr));

    const uint64_t offsetA = offsetOf(heap, a);
    const uint64_t offsetB = offsetOf(heap, b);
    CHECK(offsetA != offsetB);
    // Touching at a single subgraph is enough to keep them apart, so the ranges must be disjoint.
    CHECK((offsetA + kOneMiB <= offsetB || offsetB + kOneMiB <= offsetA));
    CHECK_FALSE(heap.isAliased(a));
    CHECK_FALSE(heap.isAliased(b));
}

TEST_CASE("VkmAliasedMemoryHeap - a resource spanning both gets its own bytes") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    const vkm::VkmResourceHandle r1 = makeHandle(1);
    const vkm::VkmResourceHandle r2 = makeHandle(2);
    const vkm::VkmResourceHandle r3 = makeHandle(3);
    REQUIRE(heap.registerResource(r1, kOneMiB, 256, ~0u));
    REQUIRE(heap.registerResource(r2, kOneMiB, 256, ~0u));
    REQUIRE(heap.registerResource(r3, kOneMiB, 256, ~0u));

    REQUIRE(heap.place({{r1, 0, 1}, {r2, 2, 3}, {r3, 0, 3}}, nullptr));

    // R1 and R2 still pair off; R3 overlaps both and must sit elsewhere.
    CHECK(offsetOf(heap, r1) == offsetOf(heap, r2));
    CHECK(offsetOf(heap, r3) != offsetOf(heap, r1));
}

TEST_CASE("VkmAliasedMemoryHeap - placement is deterministic regardless of declaration order") {
    // What the three per-frame-slot render graphs depend on: each compiles independently, and a
    // disagreement would mean one slot binding memory another slot's plan says belongs elsewhere.
    const auto placeInOrder = [](const std::vector<vkm::VkmAliasLifetime>& lifetimes) {
        FakeAliasDriver driver;
        vkm::VkmAliasedMemoryHeap heap(&driver);
        for (const vkm::VkmAliasLifetime& lifetime : lifetimes)
        {
            // Deliberately unequal sizes, so the size-descending sort has real work to do.
            REQUIRE(heap.registerResource(lifetime._handle, lifetime._handle.id * kOneMiB, 256, ~0u));
        }
        REQUIRE(heap.place(lifetimes, nullptr));

        std::vector<uint64_t> offsets;
        for (const vkm::VkmAliasLifetime& lifetime : lifetimes)
        {
            offsets.push_back(offsetOf(heap, lifetime._handle));
        }
        return offsets;
    };

    const std::vector<vkm::VkmAliasLifetime> forward{
        {makeHandle(1), 0, 1}, {makeHandle(2), 2, 3}, {makeHandle(3), 0, 3}, {makeHandle(4), 4, 5}};
    std::vector<vkm::VkmAliasLifetime> reversed(forward.rbegin(), forward.rend());

    const std::vector<uint64_t> forwardOffsets = placeInOrder(forward);
    const std::vector<uint64_t> reversedOffsets = placeInOrder(reversed);

    for (size_t i = 0; i < forward.size(); ++i)
    {
        const size_t mirrored = forward.size() - 1 - i;
        CHECK(forwardOffsets[i] == reversedOffsets[mirrored]);
    }
}

TEST_CASE("VkmAliasedMemoryHeap - an existing placement is never moved by a later place()") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    // Placement is final because Vulkan binds image memory once and VkmResourceTable bakes the
    // resulting view in; a moved resource would invalidate every table naming it.
    const vkm::VkmResourceHandle first = makeHandle(1);
    REQUIRE(heap.registerResource(first, 4 * kOneMiB, 256, ~0u));
    REQUIRE(heap.place({{first, 0, 1}}, nullptr));
    const uint64_t originalOffset = offsetOf(heap, first);

    const vkm::VkmResourceHandle second = makeHandle(2);
    REQUIRE(heap.registerResource(second, 8 * kOneMiB, 256, ~0u));
    std::vector<vkm::VkmAliasPlacement> unused;
    std::vector<vkm::VkmResourceHandle> placed;
    REQUIRE(heap.place({{first, 0, 1}, {second, 2, 3}}, &placed));

    // Only the new resource is reported, and the old one has not budged even though it is
    // smaller and would have sorted after the newcomer in a from-scratch pass.
    CHECK(placed.size() == 1);
    CHECK(placed[0] == second);
    CHECK(offsetOf(heap, first) == originalOffset);
}

TEST_CASE("VkmAliasedMemoryHeap - validate rejects lifetimes that have come to overlap") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    const vkm::VkmResourceHandle r1 = makeHandle(1);
    const vkm::VkmResourceHandle r2 = makeHandle(2);
    REQUIRE(heap.registerResource(r1, kOneMiB, 256, ~0u));
    REQUIRE(heap.registerResource(r2, kOneMiB, 256, ~0u));
    REQUIRE(heap.place({{r1, 0, 1}, {r2, 2, 3}}, nullptr));
    REQUIRE(offsetOf(heap, r1) == offsetOf(heap, r2));

    std::string error;
    CHECK(heap.validate({{r1, 0, 1}, {r2, 2, 3}}, &error));

    // The graph's shape changed after placement was frozen: R1 now lives long enough to collide
    // with R2 in bytes they already share.
    CHECK_FALSE(heap.validate({{r1, 0, 3}, {r2, 2, 3}}, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("VkmAliasedMemoryHeap - an undeclared resource is treated as live for the whole graph") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    const vkm::VkmResourceHandle declared = makeHandle(1);
    const vkm::VkmResourceHandle undeclared = makeHandle(2);
    REQUIRE(heap.registerResource(declared, kOneMiB, 256, ~0u));
    REQUIRE(heap.registerResource(undeclared, kOneMiB, 256, ~0u));

    // Only one is declared. The other must not be assumed dead -- it may well be used next
    // frame, and an empty lifetime would let the two look mutually disjoint.
    REQUIRE(heap.place({{declared, 0, 1}}, nullptr));
    CHECK(offsetOf(heap, declared) != offsetOf(heap, undeclared));
}

TEST_CASE("VkmAliasedMemoryHeap - unregistering frees the range for a later resource") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    const vkm::VkmResourceHandle first = makeHandle(1);
    REQUIRE(heap.registerResource(first, kOneMiB, 256, ~0u));
    REQUIRE(heap.place({{first, 0, 3}}, nullptr));
    const uint64_t freedOffset = offsetOf(heap, first);

    heap.unregisterResource(first);
    CHECK_FALSE(heap.getPlacement(first).has_value());

    // The range is free again, so an overlapping-lifetime newcomer may take it -- no new block.
    const vkm::VkmResourceHandle second = makeHandle(2);
    REQUIRE(heap.registerResource(second, kOneMiB, 256, ~0u));
    REQUIRE(heap.place({{second, 0, 3}}, nullptr));
    CHECK(offsetOf(heap, second) == freedOffset);
    CHECK(heap.getBlockCount() == 1);
}

TEST_CASE("VkmAliasedMemoryHeap - incompatible memory types open a second block") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    // Vulkan's memoryTypeBits: an image whose requirements exclude the block's chosen type
    // cannot be bound into it, however much room is left.
    const vkm::VkmResourceHandle a = makeHandle(1);
    const vkm::VkmResourceHandle b = makeHandle(2);
    REQUIRE(heap.registerResource(a, kOneMiB, 256, 0x1));
    REQUIRE(heap.registerResource(b, kOneMiB, 256, 0x2));
    REQUIRE(heap.place({{a, 0, 1}, {b, 2, 3}}, nullptr));

    CHECK(heap.getBlockCount() == 2);
    CHECK(heap.getPlacement(a)->_blockIndex != heap.getPlacement(b)->_blockIndex);
    // Disjoint lifetimes were not enough: they still cost two blocks.
    CHECK_FALSE(heap.isAliased(a));
}

TEST_CASE("VkmAliasedMemoryHeap - offsets respect the requested alignment") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    constexpr uint64_t kAlignment = 64 * 1024;
    const vkm::VkmResourceHandle a = makeHandle(1);
    const vkm::VkmResourceHandle b = makeHandle(2);
    // A deliberately unaligned size, so the second resource cannot simply abut the first.
    REQUIRE(heap.registerResource(a, kOneMiB + 1, kAlignment, ~0u));
    REQUIRE(heap.registerResource(b, kOneMiB, kAlignment, ~0u));
    REQUIRE(heap.place({{a, 0, 3}, {b, 0, 3}}, nullptr));

    CHECK(offsetOf(heap, a) % kAlignment == 0);
    CHECK(offsetOf(heap, b) % kAlignment == 0);
    CHECK(offsetOf(heap, a) != offsetOf(heap, b));
}

TEST_CASE("VkmAliasedMemoryHeap - a resource larger than a block gets a block sized to it") {
    FakeAliasDriver driver;
    vkm::VkmAliasedMemoryHeap heap(&driver);

    const uint64_t oversized = vkm::VkmAliasedMemoryHeap::BLOCK_SIZE_BYTES + kOneMiB;
    const vkm::VkmResourceHandle big = makeHandle(1);
    REQUIRE(heap.registerResource(big, oversized, 256, ~0u));
    REQUIRE(heap.place({{big, 0, 1}}, nullptr));

    // An attachment is one indivisible allocation, so refusing it would be fatal rather than
    // merely wasteful.
    REQUIRE(driver._blockSizes.size() == 1);
    CHECK(driver._blockSizes[0] >= oversized);
    CHECK(heap.getReservedBytes() >= oversized);
}
