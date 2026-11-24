// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <variant>
#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/descriptor_payload_growth.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {
// Each queue's host storage starts at 1/n of its size and grows on demand up to the usual ceiling,
// four times the size. EDEN_DESCRIPTOR_START_DIV=<n> sets n (1 = the full size at start).
// PS5: n = 4 by default, so the guest queue starts at 0x20000 entries (24 MiB instead of 96) and
// the compute queues at 0x8000 (6 MiB instead of 24). Peaks seen a frame: guest 48k (MKW) and
// 76-80k (Zelda), compute 5-14k, so neither grows (docs/HEAP-MEMORY-RESEARCH.md).
size_t StartPayloadSize(size_t size) {
    static const size_t divisor = [] {
#if defined(__PROSPERO__) // PS5
        constexpr long default_divisor = 4;
#else
        constexpr long default_divisor = 1;
#endif
        const char* value = std::getenv("EDEN_DESCRIPTOR_START_DIV");
        const long parsed = value ? std::strtol(value, nullptr, 10) : default_divisor;
        return static_cast<size_t>(parsed >= 1 && parsed <= 64 ? parsed : default_divisor);
    }();
    return (std::max<size_t>)(size / divisor, 0x1000);
}

// H47: frames with no growth for this many frames give their extra chunks back.
size_t ShrinkWindowFrames() {
    static const size_t frames = [] {
        const char* value = std::getenv("EDEN_DESC_SHRINK_FRAMES");
        const long parsed = value ? std::strtol(value, nullptr, 10) : 600;
        return static_cast<size_t>(parsed >= 0 ? parsed : 600); // 0 = never shrink
    }();
    return frames;
}

constexpr size_t MiB(size_t entries) {
    return entries * sizeof(DescriptorUpdateEntry) >> 20;
}

auto AllocateEntries = [](size_t count) {
    return std::make_unique_for_overwrite<DescriptorUpdateEntry[]>(count);
};
} // Anonymous namespace

UpdateDescriptorQueue::UpdateDescriptorQueue(const Device& device_, size_t frame_payload_size_,
                                             bool supports_descriptor_buffer_)
    : device{device_}, frame_payload_size{StartPayloadSize(frame_payload_size_)},
      maximum_frame_payload_size{frame_payload_size_ * 4},
      supports_descriptor_buffer{supports_descriptor_buffer_},
      payload{std::make_unique<DescriptorChunkPool<DescriptorUpdateEntry>>(
          StartPayloadSize(frame_payload_size_), frame_payload_size_ * 4 * FRAMES_IN_FLIGHT)} {}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    const size_t frame_entries =
        frame_entries_before + static_cast<size_t>(std::distance(payload_start, payload_cursor));
    peak_frame_entries = (std::max)(peak_frame_entries, frame_entries);
    window_peak_in_use = (std::max)(window_peak_in_use, payload->InUseEntries());
    // The chunk in use carries on into the next frame (chunks are retired when they fill).
    payload_start = payload_cursor;
    frame_entries_before = 0;
    // H47: after a quiet stretch, give back the free chunks no moment of the stretch needed
    // (one chunk of margin). Free chunks are read by nothing: no drain.
    const size_t shrink_window = ShrinkWindowFrames();
    if (shrink_window != 0 && ++window_frames >= shrink_window) {
        const size_t held_before = payload->HeldEntries();
        const size_t freed = payload->Trim(window_peak_in_use + frame_payload_size);
        if (freed != 0) {
            freed_total += freed;
            growth_disabled = false; // memory came back: a failed allocation may work again
            reported_held_high = payload->HeldEntries();
            LOG_INFO(Render_Vulkan, "Descriptor queue shrank: {} -> {} MiB host storage (gave back {} MiB)",
                     MiB(held_before), MiB(payload->HeldEntries()), MiB(freed));
            std::fprintf(stderr,
                         "EDEN_DESC_QUEUE shrink held_mib=%zu->%zu freed_mib=%zu freed_total_mib=%zu "
                         "window_peak_in_use_mib=%zu\n",
                         MiB(held_before), MiB(payload->HeldEntries()), MiB(freed),
                         MiB(freed_total), MiB(window_peak_in_use));
        }
        window_frames = 0;
        window_peak_in_use = 0;
    }
    // Research: the most entries one frame used, every 3,600 frames.
    if (++ticks_since_report >= 3600) {
        LOG_INFO(Render_Vulkan,
                 "Descriptor queue: peak {} entries a frame (chunk {}), host storage {} MiB, "
                 "freed {} MiB so far, worker drains {}",
                 peak_frame_entries, frame_payload_size, MiB(payload->HeldEntries()),
                 MiB(freed_total), drains);
        std::fprintf(stderr,
                     "EDEN_DESC_QUEUE report chunk=%zu peak_frame=%zu held_mib=%zu in_use_mib=%zu "
                     "recycled=%zu freed_total_mib=%zu drains=%zu worker_behind_chunks=%llu\n",
                     frame_payload_size, peak_frame_entries, MiB(payload->HeldEntries()),
                     MiB(payload->InUseEntries()), payload->RecycledChunks(), MiB(freed_total),
                     drains,
                     static_cast<unsigned long long>(frame_sequence -
                                                     worker_passed->load(std::memory_order_relaxed)));
        ticks_since_report = 0;
        peak_frame_entries = 0;
    }
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
}

/// Reserve one batch. H47b: a full chunk moves on to a chunk from the shared pool (one the worker
/// is done with, or a new one of a few MiB: no drain, no big block); only at the pool's ceiling or
/// on a failed allocation the worker is drained and every chunk reused (the old fallback).
void UpdateDescriptorQueue::Acquire(Scheduler& scheduler, size_t required_entries,
                                    bool use_descriptor_buffer_) {
    use_descriptor_buffer = supports_descriptor_buffer && use_descriptor_buffer_;
    static constexpr size_t DEFAULT_REQUIRED_ENTRIES = 0x400;
    const size_t reserve = required_entries > 0 ? required_entries : DEFAULT_REQUIRED_ENTRIES;
    if (reserve > maximum_frame_payload_size) {
        throw std::length_error("Descriptor reservation exceeds the bounded staging capacity");
    }
    if (payload_cursor == nullptr || reserve > static_cast<size_t>(chunk_end - payload_cursor)) {
        frame_entries_before += static_cast<size_t>(std::distance(payload_start, payload_cursor));
        if (payload_cursor != nullptr) {
            // The full chunk goes back to the pool once the worker ran this marker: it is recorded
            // after every command that reads the chunk (each reservation's command is recorded
            // before the next Acquire).
            ++frame_sequence;
            payload->Retire(frame_sequence);
            scheduler.Record([passed = worker_passed, sequence = frame_sequence](vk::CommandBuffer) {
                passed->store(sequence, std::memory_order_release);
            });
        }
        const u64 passed = worker_passed->load(std::memory_order_acquire);
        DescriptorUpdateEntry* next = nullptr;
        if (!growth_disabled) {
            next = payload->Take(reserve, passed, AllocateEntries);
            growth_disabled |= payload->AllocationFailed();
        }
        if (next == nullptr) {
            if (!fallback_reported) {
                LOG_WARNING(Render_Vulkan,
                            "Descriptor payload {} at {} MiB host storage; using worker drain fallback",
                            growth_disabled ? "allocation failed" : "pool limit reached",
                            MiB(payload->HeldEntries()));
                std::fprintf(stderr, "EDEN_DESC_QUEUE fallback=%s held_mib=%zu\n",
                             growth_disabled ? "allocation_failed" : "pool_limit",
                             MiB(payload->HeldEntries()));
                fallback_reported = true;
            }
            ++drains;
            scheduler.WaitWorker();
            payload->RecycleAfterDrain();
            next = payload->Take(reserve, frame_sequence, AllocateEntries);
            if (next == nullptr) {
                throw std::bad_alloc{};
            }
        }
        if (payload->HeldEntries() >= reported_held_high + 8 * frame_payload_size) {
            reported_held_high = payload->HeldEntries();
            LOG_INFO(Render_Vulkan, "Descriptor payload grew: host storage {} MiB, frame entries={}",
                     MiB(payload->HeldEntries()), frame_entries_before);
            std::fprintf(stderr,
                         "EDEN_DESC_QUEUE grow held_mib=%zu in_use_mib=%zu frame_entries=%zu "
                         "reserve=%zu worker_behind_chunks=%llu\n",
                         MiB(payload->HeldEntries()), MiB(payload->InUseEntries()),
                         frame_entries_before, reserve,
                         static_cast<unsigned long long>(frame_sequence - passed));
        }
        window_peak_in_use = (std::max)(window_peak_in_use, payload->InUseEntries());
        payload_start = next;
        payload_cursor = next;
        chunk_end = next + payload->CurrentSize();
    }
    upload_start = payload_cursor;
}

} // namespace Vulkan
