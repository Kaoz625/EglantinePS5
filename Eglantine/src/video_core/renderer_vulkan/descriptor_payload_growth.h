// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
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

/// H47: descriptor payload kept as chunks per frame slot. A frame that needs more entries moves to
/// the next chunk of its own slot (allocated on first need, no drain, no big contiguous block), so
/// only the slot that overflows grows, by one chunk at a time (= what the frame needs). Before H47 a
/// load frame of Kirby Air Riders doubled ALL 8 slots at once: 96 -> 192 -> 384 MiB in one block,
/// held for the rest of the session. Shrink() gives chunks back after a quiet stretch.
template <class Entry>
class ChunkedDescriptorPayload {
public:
    struct Chunk {
        std::unique_ptr<Entry[]> data;
        size_t size{};
    };

    /// frames = slots (frames in flight), chunk_entries = the size of each normal chunk,
    /// max_entries = the most entries one slot may hold (its chunks summed).
    ChunkedDescriptorPayload(size_t frames, size_t chunk_entries_, size_t max_entries_)
        : slots(frames), chunk_entries{chunk_entries_}, max_entries{max_entries_} {}

    /// Allocates chunk 0 of every slot. False when an allocation failed.
    template <class Allocate>
    bool Init(Allocate&& allocate) {
        for (auto& slot : slots) {
            if (!slot.empty()) {
                continue;
            }
            Chunk chunk{TryAllocate(allocate, chunk_entries), chunk_entries};
            if (!chunk.data) {
                return false;
            }
            slot.push_back(std::move(chunk));
        }
        return true;
    }

    /// Start of a frame slot: its chunk 0.
    Entry* Begin(size_t frame) {
        current_slot = frame;
        current_chunk = 0;
        return slots[frame][0].data.get();
    }

    /// After a drain: back to chunk 0 of the current slot.
    Entry* Restart() {
        current_chunk = 0;
        return slots[current_slot][0].data.get();
    }

    /// Moves the current slot to its next chunk with room for `reserve` entries, allocating it
    /// when the slot has none yet. nullptr = the slot is at its maximum or the allocation failed
    /// (allocation_failed tells which); the caller then drains and Restart()s.
    template <class Allocate>
    Entry* Next(size_t reserve, Allocate&& allocate) {
        allocation_failed = false;
        auto& slot = slots[current_slot];
        const size_t next_index = current_chunk + 1;
        const size_t want = (std::max)(chunk_entries, reserve);
        size_t before = 0;
        for (size_t i = 0; i < next_index && i < slot.size(); ++i) {
            before += slot[i].size;
        }
        if (before + want > max_entries) {
            return nullptr;
        }
        if (next_index < slot.size() && slot[next_index].size < want) {
            // Too small for this reservation: its old entries are from an earlier frame.
            held_entries -= slot[next_index].size;
            slot.erase(slot.begin() + static_cast<std::ptrdiff_t>(next_index));
        }
        if (next_index >= slot.size()) {
            Chunk chunk{TryAllocate(allocate, want), want};
            if (!chunk.data) {
                allocation_failed = true;
                return nullptr;
            }
            held_entries += want;
            slot.insert(slot.begin() + static_cast<std::ptrdiff_t>(next_index), std::move(chunk));
        }
        current_chunk = next_index;
        return slot[current_chunk].data.get();
    }

    /// Frees the chunks past `keep_chunks` in every slot, never chunk 0 and never a chunk of the
    /// current slot at or before the one in use. Call only when no queued work reads the payload
    /// (after the worker drained). Returns the entries freed.
    size_t Shrink(size_t keep_chunks) {
        keep_chunks = (std::max<size_t>)(keep_chunks, 1);
        size_t freed = 0;
        for (size_t s = 0; s < slots.size(); ++s) {
            auto& slot = slots[s];
            const size_t keep = s == current_slot ? (std::max)(keep_chunks, current_chunk + 1)
                                                  : keep_chunks;
            while (slot.size() > keep) {
                freed += slot.back().size;
                slot.pop_back();
            }
        }
        held_entries -= freed;
        return freed;
    }

    size_t CurrentChunkSize() const {
        return slots[current_slot][current_chunk].size;
    }
    size_t CurrentChunk() const {
        return current_chunk;
    }
    /// Entries held beyond chunk 0 of each slot (what growth added).
    size_t ExtraEntries() const {
        return held_entries;
    }
    /// All entries held (chunk 0 of every slot included).
    size_t HeldEntries() const {
        return held_entries + slots.size() * chunk_entries;
    }
    size_t SlotChunks(size_t slot) const {
        return slots[slot].size();
    }
    bool AllocationFailed() const {
        return allocation_failed;
    }

private:
    template <class Allocate>
    static std::unique_ptr<Entry[]> TryAllocate(Allocate& allocate, size_t count) {
        try {
            return allocate(count);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
    }

    std::vector<std::vector<Chunk>> slots;
    size_t chunk_entries;
    size_t max_entries;
    size_t current_slot{};
    size_t current_chunk{};
    size_t held_entries{};
    bool allocation_failed{};
};

} // namespace Vulkan
