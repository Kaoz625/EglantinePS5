// SPDX-FileCopyrightText: 2026 EglantinePS5
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// EDEN_NX2_PAGE_HISTORY=1 (diagnostic): the last few things that happened to each 4 KiB device
// page - CPU writes into GPU-cached pages, uploads, GPU-written marks set and cleared, downloads
// into guest memory, SMMU maps and unmaps - so a rejected native shader can say how its code page
// came to hold what it holds (Yoshi: code pages that read all zero next to good ones).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"

namespace VideoCommon::PageHistory {

enum class Event : u8 {
    None = 0,
    CpuWrite,     ///< A guest CPU write into a page the GPU caches track
    CpuMarked,    ///< Buffer cache: page marked CPU-modified (will be uploaded)
    Uploaded,     ///< Buffer cache: CPU-modified mark cleared (uploaded to the host buffer)
    GpuMarked,    ///< Buffer cache: page marked GPU-written
    GpuCleared,   ///< Buffer cache: GPU-written mark cleared
    Downloaded,   ///< Buffer cache: host buffer bytes written into guest memory
    SmmuMapped,   ///< SMMU: device page mapped to guest memory
    SmmuUnmapped, ///< SMMU: device page unmapped
};

inline const char* Name(Event event) {
    switch (event) {
    case Event::CpuWrite: return "cpu-write";
    case Event::CpuMarked: return "cpu-marked";
    case Event::Uploaded: return "uploaded";
    case Event::GpuMarked: return "gpu-marked";
    case Event::GpuCleared: return "gpu-cleared";
    case Event::Downloaded: return "downloaded";
    case Event::SmmuMapped: return "smmu-map";
    case Event::SmmuUnmapped: return "smmu-unmap";
    default: return "-";
    }
}

inline constexpr size_t SlotBits = 18; // 256 Ki pages tracked (~9 MiB)
inline constexpr size_t EventsPerSlot = 6;

struct Slot {
    std::array<std::atomic<u64>, EventsPerSlot> events{};
    std::atomic<u32> next{};
};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PAGE_HISTORY");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

inline Slot* Table() {
    static const std::unique_ptr<Slot[]> table{Enabled() ? new Slot[size_t{1} << SlotBits] : nullptr};
    return table.get();
}

inline u32 NowMs() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

// One event word: page number (28 bits) | event (4 bits) | milliseconds since start (32 bits).
inline void Record(u64 device_address, u64 size, Event event) {
    if (!Enabled() || size == 0) [[likely]] {
        return;
    }
    Slot* const table = Table();
    const u64 first = device_address >> 12;
    const u64 last = (device_address + size - 1) >> 12;
    const u64 count = (std::min)(last - first + 1, u64{1024});
    const u64 time = NowMs();
    for (u64 page = first; page < first + count; ++page) {
        Slot& slot = table[page & ((u64{1} << SlotBits) - 1)];
        const u32 index = slot.next.fetch_add(1, std::memory_order_relaxed) % EventsPerSlot;
        slot.events[index].store(((page & 0xFFFFFFF) << 36) | (u64(event) << 32) | time,
                                 std::memory_order_relaxed);
    }
}

/// The page's recorded events, oldest first, with their age in ms.
inline std::string Describe(u64 device_address) {
    if (!Enabled()) {
        return "(page history off: EDEN_NX2_PAGE_HISTORY=1)";
    }
    const u64 page = device_address >> 12;
    Slot& slot = Table()[page & ((u64{1} << SlotBits) - 1)];
    const u32 next = slot.next.load(std::memory_order_relaxed);
    const u32 now = NowMs();
    std::string text;
    for (u32 i = 0; i < EventsPerSlot; ++i) {
        const u64 word = slot.events[(next + i) % EventsPerSlot].load(std::memory_order_relaxed);
        if (word == 0 || (word >> 36) != (page & 0xFFFFFFF)) {
            continue;
        }
        const u32 time = static_cast<u32>(word);
        text += fmt::format(" {}@-{}ms", Name(static_cast<Event>((word >> 32) & 0xF)), now - time);
    }
    return text.empty() ? " (nothing recorded)" : text;
}

} // namespace VideoCommon::PageHistory
