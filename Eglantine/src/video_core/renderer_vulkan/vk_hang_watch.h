// SPDX-License-Identifier: GPL-3.0-or-later
// GPU hang watch. On the PS5 a submission that never finishes (one draw or dispatch running
// forever) blocks the whole graphics pipe: the system's own UI stops drawing, is killed ~3 s
// later ("Killing Shell UI since HP3D has timed out"), and ~45 s after that the system ends the
// app as an asynchronous GPU exception (0xa0d0c00c GPU_FAULT_SUSPENDPOINT_TIMEOUT_IN_RUN_ASYNC,
// error CE-108255-1). No signal reaches the app, so no crash report was written: the "silent
// exits" (Switch Sports: klog on DIGI5 21 Nov 2026; Fast Fusion: same CE-108255-1 code).
//
// With markers on, every draw and dispatch recorded through Scheduler::TraceGpuWork writes its
// number to a host-visible buffer when the command processor reaches it (VK_AMD_buffer_marker,
// top of pipe) and again when it has finished (bottom of pipe), and the host keeps a ring of what
// each number was (shaders, size, kind). A thread watches the timeline: when the GPU makes no
// progress on submitted work for EDEN_GPU_HANG_MS (default 1500), it prints the stuck work and
// its neighbours to the log and klog, then (EDEN_GPU_HANG_EXIT, default 1) ends the app - only
// when the markers show a draw or dispatch that started and never finished (a stall with nothing
// in flight, e.g. waiting for presentation, is reported and the app keeps running).
//
// EDEN_GPU_HANG_TRACE=0 turns the markers off (the watch still reports, without the work list).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MasterSemaphore;

enum class HangWorkKind : u8 {
    Unknown = 0,
    Draw,
    DrawIndexed,
    DrawIndirect,
    DrawIndexedIndirect,
    DrawIndirectCount,
    DrawIndexedIndirectCount,
    DrawByteCount,
    DrawMesh,
    Dispatch,
    DispatchIndirect,
};

/// A resource a draw or dispatch writes or reads, kept for the hang report (EDEN_GPU_HANG_RES=1).
struct HangResource {
    u64 host;  ///< Host GPU address (0: unknown)
    u64 size;  ///< Bytes from `host`
    u64 guest; ///< Guest GPU address
    u64 info;  ///< Images: width | height << 16 | depth << 32 | levels << 48
    u8 kind;   ///< 1 storage buffer read, 2 storage buffer written, 3 multi-range storage, 4 written image
};

class GpuHangWatch {
public:
    explicit GpuHangWatch(const Device& device, MasterSemaphore& master_semaphore);
    ~GpuHangWatch();

    GpuHangWatch(const GpuHangWatch&) = delete;
    GpuHangWatch& operator=(const GpuHangWatch&) = delete;

    /// Worker thread, right before a draw or dispatch is recorded. Returns its number (0: no
    /// markers).
    u32 Begin(vk::CommandBuffer cmdbuf, u64 hash, u64 fragment_hash, std::array<u32, 3> size,
              HangWorkKind kind) noexcept;

    /// Worker thread, right after the draw or dispatch numbered `number` was recorded.
    void End(vk::CommandBuffer cmdbuf, u32 number) noexcept;

    /// Worker thread, after a submission went to the queue.
    void NoteSubmitted(u64 tick) noexcept;

    [[nodiscard]] bool MarkersOn() const noexcept {
        return markers != nullptr;
    }

    /// True when work resources are kept for the report (EDEN_GPU_HANG_RES=1 and markers on).
    [[nodiscard]] bool ResourcesOn() const noexcept {
        return markers != nullptr && !resources.empty();
    }

    /// Worker thread, right before Begin: the resources of the next draw or dispatch.
    void NoteResources(std::span<const HangResource> list) noexcept;

    static constexpr u32 MaxResources = 6;

private:
    struct Work {
        u32 number;
        HangWorkKind kind;
        std::array<u32, 3> size;
        u64 hash;
        u64 fragment_hash;
        u64 tick; ///< Submission the work goes into (the next one after the last submitted).
    };
    static constexpr u32 RingSize = 1u << 14;

    void Watch(std::stop_token token);
    void Report(u64 gpu_tick, u64 submitted_tick, u64 stalled_ms);

    const Device& device;
    MasterSemaphore& master_semaphore;
    VkBuffer marker_buffer{};
    vk::DeviceMemory marker_memory;
    volatile u32* markers{}; ///< [0] last number the command processor reached, [1] last finished.
    std::vector<Work> ring;
    std::vector<std::array<HangResource, MaxResources>> resources; ///< Per ring slot (opt-in)
    std::vector<u8> resource_counts;
    std::array<HangResource, MaxResources> pending{};
    u32 pending_count{};
    u32 next_number{1};
    std::atomic<u64> submitted_tick{0};
    u64 limit_ms{1500};
    bool exit_on_hang{true};
    std::jthread watch_thread;
};

} // namespace Vulkan
