// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/descriptor_payload_growth.h"

using Vulkan::GrowDescriptorPayload;
using Vulkan::PayloadGrowthResult;

TEST_CASE("Descriptor growth keeps queued payload alive until drained", "[descriptor_payload]") {
    size_t capacity = 4;
    auto storage = std::make_unique<int[]>(capacity * 8);
    storage[3] = 123;
    int* const queued = storage.get() + 3;
    bool drained = false;
    const auto result = GrowDescriptorPayload(
        storage, capacity, 16, 8, 5,
        [&] {
            REQUIRE(*queued == 123);
            REQUIRE(capacity == 4);
            drained = true;
        },
        [&](size_t count) {
            REQUIRE_FALSE(drained);
            REQUIRE(count == 64);
            return std::make_unique<int[]>(count);
        });
    REQUIRE(result == PayloadGrowthResult::Grown);
    REQUIRE(drained);
    REQUIRE(capacity == 8);
}

TEST_CASE("Descriptor growth satisfies large reservations within the ceiling",
          "[descriptor_payload]") {
    size_t capacity = 4;
    auto storage = std::make_unique<int[]>(32);
    unsigned waits = 0;
    const auto result = GrowDescriptorPayload(
        storage, capacity, 16, 8, 15, [&] { ++waits; },
        [](size_t count) { return std::make_unique<int[]>(count); });
    REQUIRE(result == PayloadGrowthResult::Grown);
    REQUIRE(capacity == 16);
    REQUIRE(waits == 1);
    storage[capacity * 8 - 1] = 42;
    REQUIRE(storage[127] == 42);
    REQUIRE(GrowDescriptorPayload(
                storage, capacity, 16, 8, 17, [&] { ++waits; },
                [](size_t count) { return std::make_unique<int[]>(count); }) ==
            PayloadGrowthResult::AtLimit);
    REQUIRE(waits == 1);
}

TEST_CASE("Descriptor allocation failure preserves reusable storage", "[descriptor_payload]") {
    size_t capacity = 4;
    auto storage = std::make_unique<int[]>(32);
    storage[0] = 99;
    int* const original = storage.get();
    unsigned waits = 0;
    REQUIRE(GrowDescriptorPayload(
                storage, capacity, 16, 8, 5, [&] { ++waits; },
                [](size_t) -> std::unique_ptr<int[]> { throw std::bad_alloc{}; }) ==
            PayloadGrowthResult::AllocationFailed);
    REQUIRE(capacity == 4);
    REQUIRE(storage.get() == original);
    REQUIRE(storage[0] == 99);
    REQUIRE(waits == 0);
}

TEST_CASE("Descriptor growth clips doubling to its configured ceiling", "[descriptor_payload]") {
    size_t capacity = 8;
    auto storage = std::make_unique<int[]>(64);
    REQUIRE(GrowDescriptorPayload(
                storage, capacity, 12, 8, 9, [] {},
                [](size_t count) {
                    REQUIRE(count == 96);
                    return std::make_unique<int[]>(count);
                }) == PayloadGrowthResult::Grown);
    REQUIRE(capacity == 12);
    REQUIRE(GrowDescriptorPayload(
                storage, capacity, 12, 8, 12, [] { FAIL("Must not drain at the ceiling"); },
                [](size_t) -> std::unique_ptr<int[]> {
                    FAIL("Must not allocate at ceiling");
                    return {};
                }) == PayloadGrowthResult::AtLimit);
}

using Vulkan::DescriptorChunkPool;

namespace {
auto AllocInts = [](size_t count) { return std::make_unique<int[]>(count); };
}

TEST_CASE("Descriptor pool reuses a chunk only after the worker passed it", "[descriptor_payload]") {
    DescriptorChunkPool<int> pool(4, 64);
    int* const first = pool.Take(1, 0, AllocInts);
    REQUIRE(first != nullptr);
    first[0] = 11;
    pool.Retire(1);
    int* const second = pool.Take(1, 0, AllocInts); // worker has not passed 1: a new chunk
    REQUIRE(second != nullptr);
    REQUIRE(second != first);
    REQUIRE(first[0] == 11);
    REQUIRE(pool.HeldEntries() == 8);
    pool.Retire(2);
    int* const third = pool.Take(1, 1, AllocInts); // worker passed 1: the first chunk again
    REQUIRE(third == first);
    REQUIRE(pool.HeldEntries() == 8);
    REQUIRE(pool.RecycledChunks() == 1);
    REQUIRE(pool.InUseEntries() == 8); // chunk 2 retired (not passed) + the one in use
}

TEST_CASE("Descriptor pool stops at its ceiling; a drain frees every chunk", "[descriptor_payload]") {
    DescriptorChunkPool<int> pool(4, 12);
    REQUIRE(pool.Take(1, 0, AllocInts) != nullptr);
    pool.Retire(1);
    REQUIRE(pool.Take(1, 0, AllocInts) != nullptr);
    pool.Retire(2);
    REQUIRE(pool.Take(1, 0, AllocInts) != nullptr);
    pool.Retire(3);
    REQUIRE(pool.Take(1, 0, AllocInts) == nullptr);
    REQUIRE_FALSE(pool.AllocationFailed());
    pool.RecycleAfterDrain();
    REQUIRE(pool.InUseEntries() == 0);
    unsigned allocations = 0;
    REQUIRE(pool.Take(1, 0, [&](size_t count) {
        ++allocations;
        return std::make_unique<int[]>(count);
    }) != nullptr);
    REQUIRE(allocations == 0);
}

TEST_CASE("Descriptor pool fits a reservation larger than a chunk", "[descriptor_payload]") {
    DescriptorChunkPool<int> pool(4, 64);
    REQUIRE(pool.Take(1, 0, AllocInts) != nullptr);
    pool.Retire(1);
    REQUIRE(pool.Take(10, 1, AllocInts) != nullptr); // the free 4-entry chunk is too small
    REQUIRE(pool.CurrentSize() == 10);
    REQUIRE(pool.HeldEntries() == 14);
}

TEST_CASE("Descriptor pool allocation failure is reported", "[descriptor_payload]") {
    DescriptorChunkPool<int> pool(4, 64);
    REQUIRE(pool.Take(1, 0, [](size_t) -> std::unique_ptr<int[]> { throw std::bad_alloc{}; }) ==
            nullptr);
    REQUIRE(pool.AllocationFailed());
    REQUIRE(pool.HeldEntries() == 0);
}

TEST_CASE("Descriptor pool trims only free chunks", "[descriptor_payload]") {
    DescriptorChunkPool<int> pool(4, 64);
    for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
        REQUIRE(pool.Take(1, 0, AllocInts) != nullptr);
        pool.Retire(sequence);
    }
    int* const in_use = pool.Take(1, 6, AllocInts); // all 6 passed: one reused, 5 free
    REQUIRE(in_use != nullptr);
    in_use[0] = 3;
    REQUIRE(pool.HeldEntries() == 24);
    REQUIRE(pool.Trim(8) == 16); // held 24 -> 8: four free chunks go
    REQUIRE(pool.HeldEntries() == 8);
    REQUIRE(pool.Trim(0) == 4); // the last free chunk; the one in use stays
    REQUIRE(pool.HeldEntries() == 4);
    REQUIRE(in_use[0] == 3);
}
