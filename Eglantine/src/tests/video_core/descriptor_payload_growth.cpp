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

using Vulkan::ChunkedDescriptorPayload;

namespace {
auto AllocInts = [](size_t count) { return std::make_unique<int[]>(count); };
}

TEST_CASE("Chunked payload grows only the overflowing slot, one chunk at a time",
          "[descriptor_payload]") {
    ChunkedDescriptorPayload<int> payload(8, 4, 16);
    REQUIRE(payload.Init(AllocInts));
    REQUIRE(payload.HeldEntries() == 32);
    int* const first = payload.Begin(2);
    first[0] = 7;
    int* const second = payload.Next(2, AllocInts);
    REQUIRE(second != nullptr);
    REQUIRE(second != first);
    REQUIRE(first[0] == 7); // earlier chunk of the frame stays alive (queued work reads it)
    REQUIRE(payload.CurrentChunk() == 1);
    REQUIRE(payload.ExtraEntries() == 4);
    REQUIRE(payload.HeldEntries() == 36);
    REQUIRE(payload.SlotChunks(2) == 2);
    REQUIRE(payload.SlotChunks(3) == 1);
}

TEST_CASE("Chunked payload reuses chunks and stops at the slot ceiling", "[descriptor_payload]") {
    ChunkedDescriptorPayload<int> payload(2, 4, 12);
    REQUIRE(payload.Init(AllocInts));
    payload.Begin(0);
    REQUIRE(payload.Next(1, AllocInts) != nullptr);
    REQUIRE(payload.Next(1, AllocInts) != nullptr);
    REQUIRE(payload.Next(1, AllocInts) == nullptr); // 3 chunks x 4 = 12 = ceiling
    REQUIRE_FALSE(payload.AllocationFailed());
    REQUIRE(payload.Restart() != nullptr);
    REQUIRE(payload.CurrentChunk() == 0);
    unsigned allocations = 0;
    REQUIRE(payload.Next(1, [&](size_t count) {
        ++allocations;
        return std::make_unique<int[]>(count);
    }) != nullptr);
    REQUIRE(allocations == 0); // chunk 1 already there
}

TEST_CASE("Chunked payload fits a reservation larger than a chunk", "[descriptor_payload]") {
    ChunkedDescriptorPayload<int> payload(2, 4, 32);
    REQUIRE(payload.Init(AllocInts));
    payload.Begin(1);
    REQUIRE(payload.Next(1, AllocInts) != nullptr);
    REQUIRE(payload.CurrentChunkSize() == 4);
    payload.Restart();
    REQUIRE(payload.Next(10, AllocInts) != nullptr); // chunk 1 (4) too small -> replaced
    REQUIRE(payload.CurrentChunkSize() == 10);
    REQUIRE(payload.ExtraEntries() == 10);
}

TEST_CASE("Chunked payload allocation failure leaves the slot usable", "[descriptor_payload]") {
    ChunkedDescriptorPayload<int> payload(2, 4, 32);
    REQUIRE(payload.Init(AllocInts));
    int* const base = payload.Begin(0);
    REQUIRE(payload.Next(1, [](size_t) -> std::unique_ptr<int[]> { throw std::bad_alloc{}; }) ==
            nullptr);
    REQUIRE(payload.AllocationFailed());
    REQUIRE(payload.ExtraEntries() == 0);
    REQUIRE(payload.Restart() == base);
}

TEST_CASE("Chunked payload shrink keeps chunk 0 and the chunks in use", "[descriptor_payload]") {
    ChunkedDescriptorPayload<int> payload(3, 4, 64);
    REQUIRE(payload.Init(AllocInts));
    payload.Begin(0);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(payload.Next(1, AllocInts) != nullptr);
    }
    payload.Begin(1);
    int* const in_use = payload.Next(1, AllocInts);
    REQUIRE(in_use != nullptr);
    in_use[0] = 5;
    REQUIRE(payload.ExtraEntries() == 16);
    REQUIRE(payload.Shrink(1) == 12); // slot 0 back to 1 chunk; slot 1 keeps its chunk in use
    REQUIRE(payload.SlotChunks(0) == 1);
    REQUIRE(payload.SlotChunks(1) == 2);
    REQUIRE(in_use[0] == 5);
    REQUIRE(payload.ExtraEntries() == 4);
    payload.Begin(2);
    REQUIRE(payload.Shrink(0) == 4); // keep_chunks 0 is read as 1
    REQUIRE(payload.HeldEntries() == 12);
}
