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
      payload{std::make_unique<ChunkedDescriptorPayload<DescriptorUpdateEntry>>(
          FRAMES_IN_FLIGHT, StartPayloadSize(frame_payload_size_), frame_payload_size_ * 4)} {
    if (!payload->Init(AllocateEntries)) {
        throw std::bad_alloc{};
    }
    payload_start = payload->Begin(0);
    payload_cursor = payload_start;
    chunk_end = payload_start + payload->CurrentChunkSize();
}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    const size_t frame_entries =
        frame_entries_before + static_cast<size_t>(std::distance(payload_start, payload_cursor));
    peak_frame_entries = (std::max)(peak_frame_entries, frame_entries);
    window_peak_chunks = (std::max)(window_peak_chunks, frame_chunks);
    // H47: after a quiet stretch, give back the chunks no frame of the stretch needed.
    const size_t shrink_window = ShrinkWindowFrames();
    if (shrink_window != 0 && ++window_frames >= shrink_window) {
        if (payload->ExtraEntries() != 0) {
            size_t most_chunks = 1;
            for (size_t slot = 0; slot < FRAMES_IN_FLIGHT; ++slot) {
                most_chunks = (std::max)(most_chunks, payload->SlotChunks(slot));
            }
            if (most_chunks > window_peak_chunks) {
                shrink_to_chunks = window_peak_chunks;
            }
        }
        window_frames = 0;
        window_peak_chunks = 1;
    }
    // Research: the most entries one frame used, every 3,600 frames.
    if (++ticks_since_report >= 3600) {
        LOG_INFO(Render_Vulkan,
                 "Descriptor queue: peak {} entries a frame (chunk {}), host storage {} MiB, "
                 "freed {} MiB so far, worker drains {}",
                 peak_frame_entries, frame_payload_size, MiB(payload->HeldEntries()),
                 MiB(freed_total), drains);
        std::fprintf(stderr,
                     "EDEN_DESC_QUEUE report chunk=%zu peak_frame=%zu held_mib=%zu extra_mib=%zu "
                     "freed_total_mib=%zu drains=%zu\n",
                     frame_payload_size, peak_frame_entries, MiB(payload->HeldEntries()),
                     MiB(payload->ExtraEntries()), MiB(freed_total), drains);
        ticks_since_report = 0;
        peak_frame_entries = 0;
    }
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    payload_start = payload->Begin(frame_index);
    payload_cursor = payload_start;
    chunk_end = payload_start + payload->CurrentChunkSize();
    frame_entries_before = 0;
    frame_chunks = 1;
}

/// Reserve one batch. H47: a full chunk moves on to the next chunk of the same frame slot (made on
/// first need, a few MiB, no drain); only past the slot's ceiling or on a failed allocation the
/// worker is drained and the slot reused from its start (the old fallback).
void UpdateDescriptorQueue::Acquire(Scheduler& scheduler, size_t required_entries,
                                    bool use_descriptor_buffer_) {
    use_descriptor_buffer = supports_descriptor_buffer && use_descriptor_buffer_;
    static constexpr size_t DEFAULT_REQUIRED_ENTRIES = 0x400;
    const size_t reserve = required_entries > 0 ? required_entries : DEFAULT_REQUIRED_ENTRIES;
    if (reserve > maximum_frame_payload_size) {
        throw std::length_error("Descriptor reservation exceeds the bounded staging capacity");
    }
    if (shrink_to_chunks != 0) {
        const size_t keep = shrink_to_chunks;
        shrink_to_chunks = 0;
        const size_t held_before = payload->HeldEntries();
        scheduler.WaitWorker();
        const size_t freed = payload->Shrink(keep);
        freed_total += freed;
        if (freed != 0) {
            growth_disabled = false; // memory came back: a failed allocation may work again
            reported_extra_high = payload->ExtraEntries();
            LOG_INFO(Render_Vulkan, "Descriptor queue shrank: {} -> {} MiB host storage (gave back {} MiB)",
                     MiB(held_before), MiB(payload->HeldEntries()), MiB(freed));
            std::fprintf(stderr,
                         "EDEN_DESC_QUEUE shrink held_mib=%zu->%zu freed_mib=%zu freed_total_mib=%zu "
                         "keep_chunks=%zu\n",
                         MiB(held_before), MiB(payload->HeldEntries()), MiB(freed),
                         MiB(freed_total), keep);
        }
    }
    if (payload_cursor + reserve > chunk_end) {
        frame_entries_before += static_cast<size_t>(std::distance(payload_start, payload_cursor));
        DescriptorUpdateEntry* next = nullptr;
        if (!growth_disabled) {
            next = payload->Next(reserve, AllocateEntries);
            growth_disabled |= payload->AllocationFailed();
        }
        if (next != nullptr) {
            frame_chunks = (std::max)(frame_chunks, payload->CurrentChunk() + 1);
            if (payload->ExtraEntries() > reported_extra_high) {
                reported_extra_high = payload->ExtraEntries();
                LOG_INFO(Render_Vulkan,
                         "Descriptor payload grew: slot {} chunk {} ({} entries), host storage {} MiB, "
                         "frame entries={}, reserve={}",
                         frame_index, payload->CurrentChunk(), payload->CurrentChunkSize(),
                         MiB(payload->HeldEntries()), frame_entries_before, reserve);
                std::fprintf(stderr,
                             "EDEN_DESC_QUEUE grow slot=%zu chunk=%zu held_mib=%zu frame_entries=%zu "
                             "reserve=%zu\n",
                             frame_index, payload->CurrentChunk(), MiB(payload->HeldEntries()),
                             frame_entries_before, reserve);
            }
        } else {
            if (!fallback_reported) {
                LOG_WARNING(Render_Vulkan,
                            "Descriptor payload {} at {} MiB host storage; using worker drain fallback",
                            growth_disabled ? "allocation failed" : "slot limit reached",
                            MiB(payload->HeldEntries()));
                std::fprintf(stderr, "EDEN_DESC_QUEUE fallback=%s held_mib=%zu\n",
                             growth_disabled ? "allocation_failed" : "slot_limit",
                             MiB(payload->HeldEntries()));
                fallback_reported = true;
            }
            ++drains;
            scheduler.WaitWorker();
            next = payload->Restart();
            if (reserve > payload->CurrentChunkSize()) {
                throw std::bad_alloc{};
            }
        }
        payload_start = next;
        payload_cursor = next;
        chunk_end = next + payload->CurrentChunkSize();
    }
    upload_start = payload_cursor;
}

} // namespace Vulkan
