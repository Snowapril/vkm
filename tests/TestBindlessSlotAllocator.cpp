#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/bindless_resource_manager.h>

#include <cstdint>

TEST_CASE("BindlessSlotAllocator allocates monotonically increasing slots")
{
    vkm::VkmBindlessSlotAllocator allocator(4);

    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == 1);
    CHECK(allocator.allocate() == 2);
    CHECK(allocator.allocate() == 3);
}

TEST_CASE("BindlessSlotAllocator returns UINT32_MAX when exhausted")
{
    vkm::VkmBindlessSlotAllocator allocator(2);

    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == 1);
    CHECK(allocator.allocate() == UINT32_MAX);
    CHECK(allocator.allocate() == UINT32_MAX);
}

TEST_CASE("BindlessSlotAllocator reuses released slots LIFO before the high-water mark")
{
    vkm::VkmBindlessSlotAllocator allocator(8);

    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == 1);
    CHECK(allocator.allocate() == 2);

    allocator.release(0);
    allocator.release(2);

    CHECK(allocator.allocate() == 2); // most recently released first
    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == 3); // free-list empty again -> high-water resumes
}

TEST_CASE("BindlessSlotAllocator release makes an exhausted allocator usable again")
{
    vkm::VkmBindlessSlotAllocator allocator(1);

    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == UINT32_MAX);

    allocator.release(0);
    CHECK(allocator.allocate() == 0);
    CHECK(allocator.allocate() == UINT32_MAX);
}

TEST_CASE("BindlessSlotAllocator with zero capacity always fails")
{
    vkm::VkmBindlessSlotAllocator allocator(0);

    CHECK(allocator.allocate() == UINT32_MAX);
}
