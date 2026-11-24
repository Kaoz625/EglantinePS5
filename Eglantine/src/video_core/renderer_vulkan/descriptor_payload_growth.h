// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace Vulkan {

enum class PayloadGrowthResult { Grown, AtLimit, AllocationFailed };

/// Grow within the configured ceiling, preserving old entries until drain() completes.
/// Allocation failure leaves storage and capacity unchanged; drained entries need no copy.
/// (H46 and before: the whole queue grew this way, by doubling. H47 uses ChunkedDescriptorPayload.)
template <class Entry, class Drain, class Allocate>
PayloadGrowthResult GrowDescriptorPayload(std::unique_ptr<Entry[]>& storage, size_t& capacity,
                                          size_t maximum, size_t frames, size_t required,
                                          Drain&& drain, Allocate&& allocate) {
    if (capacity == 0 || capacity >= maximum || required > maximum || frames == 0) {
        return PayloadGrowthResult::AtLimit;
    }
    size_t next = capacity;
    do {
        next += (std::min)(next, maximum - next);
    } while (next < required && next < maximum);
    if (next == 0 || next > std::numeric_limits<size_t>::max() / frames) {
        return PayloadGrowthResult::AtLimit;
    }
    std::unique_ptr<Entry[]> replacement;
    try {
        replacement = allocate(next * frames);
    } catch (const std::bad_alloc&) {
        return PayloadGrowthResult::AllocationFailed;
    }
    if (!replacement) {
        return PayloadGrowthResult::AllocationFailed;
    }
    drain();
    storage = std::move(replacement);
    capacity = next;
    return PayloadGrowthResult::Grown;
}

/// H47b: descriptor payload as one pool of chunks shared by all frames. A frame takes chunks as it
/// fills them; at its end they are retired with the frame's sequence number, and they go back to the
/// free list once the scheduler's worker has run past that frame (the worker reads the payload when
/// it executes the recorded commands; nothing reads it after that). Memory follows the frames the
/// worker is behind (1-2), not 8 frame slots: Kirby Air Riders fills ~1.5 M entries (36 MiB) every
/// frame, so 8 slots held 300-384 MiB (H45 doubling, H47 per-slot chunks).
template <class Entry>
class DescriptorChunkPool {
public:
    struct Chunk {
        std::unique_ptr<Entry[]> data;
        size_t size{};
    };

    /// chunk_entries = the size of a normal chunk; max_entries = the most entries the pool holds.
    DescriptorChunkPool(size_t chunk_entries_, size_t max_entries_)
        : chunk_entries{chunk_entries_}, max_entries{max_entries_} {}

    /// A chunk for the current frame with room for `reserve` entries: a free one, one whose frame
    /// the worker passed (sequence <= consumed), or a new one. nullptr = the pool is at its maximum
    /// or the allocation failed (AllocationFailed()); the caller drains the worker and calls
    /// RecycleAfterDrain().
    template <class Allocate>
    Entry* Take(size_t reserve, std::uint64_t consumed, Allocate&& allocate) {
        allocation_failed = false;
        Recycle(consumed);
        const size_t want = (std::max)(chunk_entries, reserve);
        for (size_t i = free.size(); i-- > 0;) {
            if (free[i].size >= reserve) {
                frame.push_back(std::move(free[i]));
                free.erase(free.begin() + static_cast<std::ptrdiff_t>(i));
                ++recycled;
                return frame.back().data.get();
            }
        }
        if (held + want > max_entries) {
            return nullptr;
        }
        std::unique_ptr<Entry[]> data;
        try {
            data = allocate(want);
        } catch (const std::bad_alloc&) {
        }
        if (!data) {
            allocation_failed = true;
            return nullptr;
        }
        held += want;
        frame.push_back(Chunk{std::move(data), want});
        return frame.back().data.get();
    }

    /// End of a frame: its chunks wait for the worker to pass `sequence`.
    void Retire(std::uint64_t sequence) {
        if (frame.empty()) {
            return;
        }
        for (Chunk& chunk : frame) {
            retired.push_back(Retired{std::move(chunk), sequence});
        }
        frame.clear();
    }

    /// After the worker drained: every chunk is free, the current frame's too (its entries were
    /// read; the caller starts a new chunk).
    void RecycleAfterDrain() {
        for (Retired& entry : retired) {
            free.push_back(std::move(entry.chunk));
        }
        retired.clear();
        for (Chunk& chunk : frame) {
            free.push_back(std::move(chunk));
        }
        frame.clear();
    }

    /// Frees free chunks while the pool holds more than `keep_entries`. Returns the entries freed.
    size_t Trim(size_t keep_entries) {
        size_t freed = 0;
        while (!free.empty() && held > keep_entries) {
            held -= free.back().size;
            freed += free.back().size;
            free.pop_back();
        }
        return freed;
    }

    size_t CurrentSize() const {
        return frame.empty() ? 0 : frame.back().size;
    }
    /// Entries in chunks the current frame or the worker may still read.
    size_t InUseEntries() const {
        size_t total = 0;
        for (const Chunk& chunk : frame) {
            total += chunk.size;
        }
        for (const Retired& entry : retired) {
            total += entry.chunk.size;
        }
        return total;
    }
    size_t HeldEntries() const {
        return held;
    }
    size_t RecycledChunks() const {
        return recycled;
    }
    bool AllocationFailed() const {
        return allocation_failed;
    }

private:
    struct Retired {
        Chunk chunk;
        std::uint64_t sequence;
    };

    void Recycle(std::uint64_t consumed) {
        size_t moved = 0;
        while (moved < retired.size() && retired[moved].sequence <= consumed) {
            free.push_back(std::move(retired[moved].chunk));
            ++moved;
        }
        retired.erase(retired.begin(), retired.begin() + static_cast<std::ptrdiff_t>(moved));
    }

    size_t chunk_entries;
    size_t max_entries;
    size_t held{};
    size_t recycled{};
    std::vector<Chunk> frame;
    std::vector<Chunk> free;
    std::vector<Retired> retired; ///< oldest first (sequences only grow)
    bool allocation_failed{};
};

} // namespace Vulkan
