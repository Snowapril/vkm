// Copyright (c) 2025 Snowapril

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/bindless_resource_manager.h>

#include <cstdint>
#include <set>

// The Metal and WebGPU push-constant rings have no push-constant instruction to lower to, so every
// setPushConstants() takes a ring entry. These cases pin the two properties that make reusing an
// entry safe: each frame slot owns a disjoint region, and a slot's cursor rewinds when that slot
// comes round again -- which the engine only does after waiting on that slot's render graph.

TEST_CASE("PushConstantRingAllocator hands out consecutive entries within a frame")
{
    vkm::VkmPushConstantRingAllocator allocator;
    allocator.beginFrame(0);

    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == 1);
    CHECK(allocator.allocate() == 2);
}

TEST_CASE("PushConstantRingAllocator gives every frame slot a disjoint region")
{
    vkm::VkmPushConstantRingAllocator allocator;
    std::set<uint32_t> seen;

    for (uint32_t frameSlot = 0; frameSlot < vkm::FRAME_BUFFER_COUNT; ++frameSlot)
    {
        allocator.beginFrame(frameSlot);
        for (uint32_t i = 0; i < 4; ++i)
        {
            const uint32_t entry = allocator.allocate();
            // Disjoint regions are the whole point: an entry a frame in flight still references
            // must not be handed to the frame recording now.
            CHECK(seen.insert(entry).second);
            CHECK(entry < vkm::kVkmPushConstantRingTotalEntryCount);
        }
    }
}

TEST_CASE("PushConstantRingAllocator rewinds a slot's region when the slot comes round again")
{
    vkm::VkmPushConstantRingAllocator allocator;

    allocator.beginFrame(1);
    const uint32_t first = allocator.allocate();
    const uint32_t second = allocator.allocate();

    // Two other slots' worth of frames go by, then slot 1 is recorded again.
    allocator.beginFrame(2);
    allocator.allocate();
    allocator.beginFrame(0);
    allocator.allocate();
    allocator.beginFrame(1);

    // Without the rewind the cursor would keep climbing and eventually run off the region, so
    // this is the assertion that fails if beginFrame() stops resetting.
    CHECK(allocator.allocate() == first);
    CHECK(allocator.allocate() == second);
}

TEST_CASE("PushConstantRingAllocator reports overflow instead of leaving its region")
{
    vkm::VkmPushConstantRingAllocator allocator;
    allocator.beginFrame(1);

    bool overflowed = false;
    uint32_t last = 0;
    for (uint32_t i = 0; i < vkm::kVkmPushConstantRingEntryCount; ++i)
    {
        last = allocator.allocate(&overflowed);
        REQUIRE_FALSE(overflowed);
    }
    CHECK(last == 2u * vkm::kVkmPushConstantRingEntryCount - 1u);

    // One past the region's capacity wraps onto this same frame's entry 0 and says so. It must
    // not spill into the neighbouring slot's region, which a frame in flight may be using.
    const uint32_t wrapped = allocator.allocate(&overflowed);
    CHECK(overflowed);
    CHECK(wrapped == vkm::kVkmPushConstantRingEntryCount);
}

TEST_CASE("PushConstantRingAllocator starts in region 0 without a beginFrame call")
{
    // Unit tests drive command buffers without an engine, so nothing calls beginFrame() there;
    // that path has to keep behaving like the single-region ring it used to be.
    vkm::VkmPushConstantRingAllocator allocator;

    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == 1);
}
