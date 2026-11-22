// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_hang_watch.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_device.h"

#if defined(__PROSPERO__)
extern "C" int sceKernelDebugOutText(int channel, const char* text) __attribute__((weak));
extern "C" int eden_exit_app(void) __attribute__((weak));
#endif

namespace Vulkan {
namespace {

u64 EnvNumber(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end == value ? fallback : static_cast<u64>(parsed);
}

const char* KindName(HangWorkKind kind) {
    switch (kind) {
    case HangWorkKind::Draw:
        return "draw";
    case HangWorkKind::DrawIndexed:
        return "draw-indexed";
    case HangWorkKind::DrawIndirect:
        return "draw-indirect";
    case HangWorkKind::DrawIndexedIndirect:
        return "draw-indexed-indirect";
    case HangWorkKind::DrawIndirectCount:
        return "draw-indirect-count";
    case HangWorkKind::DrawIndexedIndirectCount:
        return "draw-indexed-indirect-count";
    case HangWorkKind::DrawByteCount:
        return "draw-byte-count";
    case HangWorkKind::DrawMesh:
        return "draw-mesh";
    case HangWorkKind::Dispatch:
        return "dispatch";
    case HangWorkKind::DispatchIndirect:
        return "dispatch-indirect";
    case HangWorkKind::Unknown:
        break;
    }
    return "work";
}

// One line to the log file and, on the console, straight to klog: the log thread's buffer can be
// lost when the system ends the app.
void Say(const char* line) {
    std::fputs(line, stderr);
#if defined(__PROSPERO__)
    if (sceKernelDebugOutText != nullptr) {
        sceKernelDebugOutText(0, line);
    }
#endif
}

} // namespace

GpuHangWatch::GpuHangWatch(const Device& device_, MasterSemaphore& master_semaphore_)
    : device{device_}, master_semaphore{master_semaphore_} {
    limit_ms = std::max<u64>(EnvNumber("EDEN_GPU_HANG_MS", 1500), 200);
    exit_on_hang = EnvNumber("EDEN_GPU_HANG_EXIT", 1) != 0;
    const bool want_markers = EnvNumber("EDEN_GPU_HANG_TRACE", 1) != 0;
    if (want_markers && device.IsAmdBufferMarkerSupported() &&
        device.GetDispatchLoader().vkCmdWriteBufferMarkerAMD != nullptr) {
        const auto& dld = device.GetDispatchLoader();
        const VkDevice logical = *device.GetLogical();
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = 256,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        if (dld.vkCreateBuffer(logical, &buffer_ci, nullptr, &marker_buffer) == VK_SUCCESS) {
            const VkMemoryRequirements requirements =
                device.GetLogical().GetBufferMemoryRequirements(marker_buffer);
            const auto properties = device.GetPhysical().GetMemoryProperties().memoryProperties;
            constexpr VkMemoryPropertyFlags wanted =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            u32 type = properties.memoryTypeCount;
            for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
                if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                    (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                    type = i;
                    break;
                }
            }
            if (type != properties.memoryTypeCount) {
                marker_memory = device.GetLogical().TryAllocateMemory(VkMemoryAllocateInfo{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .pNext = nullptr,
                    .allocationSize = requirements.size,
                    .memoryTypeIndex = type,
                });
            }
            void* mapped = nullptr;
            if (marker_memory &&
                dld.vkBindBufferMemory(logical, marker_buffer, *marker_memory, 0) == VK_SUCCESS &&
                dld.vkMapMemory(logical, *marker_memory, 0, VK_WHOLE_SIZE, 0, &mapped) ==
                    VK_SUCCESS) {
                markers = static_cast<volatile u32*>(mapped);
                markers[0] = 0;
                markers[1] = 0;
                ring.resize(RingSize);
            }
        }
    }
    LOG_INFO(Render_Vulkan, "EDEN_GPU_HANG watch on: limit {} ms, markers {}, exit {}", limit_ms,
             markers != nullptr ? "on" : "off", exit_on_hang ? 1 : 0);
    watch_thread = std::jthread([this](std::stop_token token) { Watch(token); });
}

GpuHangWatch::~GpuHangWatch() {
    watch_thread.request_stop();
    if (watch_thread.joinable()) {
        watch_thread.join();
    }
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    if (markers != nullptr && marker_memory) {
        dld.vkUnmapMemory(logical, *marker_memory);
    }
    if (marker_buffer != VK_NULL_HANDLE) {
        dld.vkDestroyBuffer(logical, marker_buffer, nullptr);
    }
}

u32 GpuHangWatch::Begin(vk::CommandBuffer cmdbuf, u64 hash, u64 fragment_hash,
                        std::array<u32, 3> size, HangWorkKind kind) noexcept {
    if (markers == nullptr) {
        return 0;
    }
    const u32 number = next_number++;
    if (next_number == 0) {
        next_number = 1;
    }
    ring[number % RingSize] = Work{
        .number = number,
        .kind = kind,
        .size = size,
        .hash = hash,
        .fragment_hash = fragment_hash,
        .tick = submitted_tick.load(std::memory_order_relaxed) + 1,
    };
    device.GetDispatchLoader().vkCmdWriteBufferMarkerAMD(
        *cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, marker_buffer, 0, number);
    return number;
}

void GpuHangWatch::End(vk::CommandBuffer cmdbuf, u32 number) noexcept {
    if (number == 0 || markers == nullptr) {
        return;
    }
    device.GetDispatchLoader().vkCmdWriteBufferMarkerAMD(
        *cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, marker_buffer, 4, number);
}

void GpuHangWatch::NoteSubmitted(u64 tick) noexcept {
    submitted_tick.store(tick, std::memory_order_release);
}

void GpuHangWatch::Watch(std::stop_token token) {
    using Clock = std::chrono::steady_clock;
    u64 last_gpu = 0;
    auto progress = Clock::now();
    bool reported = false;
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const u64 submitted = submitted_tick.load(std::memory_order_acquire);
        try {
            master_semaphore.Refresh();
        } catch (...) {
            return; // device lost: reported elsewhere
        }
        const u64 gpu = master_semaphore.KnownGpuTick();
        const auto now = Clock::now();
        if (gpu >= submitted || gpu != last_gpu) {
            // Idle, or the GPU finished something since the last look.
            last_gpu = gpu;
            progress = now;
            reported = false;
            continue;
        }
        const u64 stalled_ms = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - progress).count());
        if (stalled_ms < limit_ms || reported) {
            continue;
        }
        reported = true;
        Report(gpu, submitted, stalled_ms);
        // Ending the app only for a draw or dispatch the GPU started and never finished. A stall
        // with nothing in flight (a submission waiting on a semaphore, e.g. for presentation)
        // is only reported.
        const bool work_stuck = markers != nullptr && markers[0] != markers[1];
        if (exit_on_hang && work_stuck) {
            std::fflush(stderr);
            std::fflush(stdout);
#if defined(__PROSPERO__)
            if (eden_exit_app != nullptr) {
                Say("EDEN_GPU_HANG ending the app (EDEN_GPU_HANG_EXIT=0 keeps it running)\n");
                std::fflush(stderr);
                eden_exit_app();
                Say("EDEN_GPU_HANG exit was refused\n");
            }
#endif
        }
    }
}

void GpuHangWatch::Report(u64 gpu_tick, u64 submitted, u64 stalled_ms) {
    char line[512];
    const u32 reached = markers != nullptr ? markers[0] : 0;
    const u32 finished = markers != nullptr ? markers[1] : 0;
    std::snprintf(line, sizeof(line),
                  "EDEN_GPU_HANG no GPU progress for %" PRIu64 " ms: submitted tick %" PRIu64
                  ", GPU at tick %" PRIu64 "; work reached=%u finished=%u (stuck: #%u)\n",
                  stalled_ms, submitted, gpu_tick, reached, finished, finished + 1);
    Say(line);
    if (markers == nullptr) {
        Say("EDEN_GPU_HANG markers off: no work list (EDEN_GPU_HANG_TRACE, VK_AMD_buffer_marker)\n");
        std::fflush(stderr);
        return;
    }
    // The work around the stuck one: a few that finished, then everything the command processor
    // had reached (bounded).
    const u32 first = finished > 4 ? finished - 4 : 1;
    u32 last = std::max(reached, finished + 1) + 2;
    last = std::min(last, first + 40);
    for (u32 number = first; number <= last; ++number) {
        const Work& work = ring[number % RingSize];
        if (work.number != number) {
            continue;
        }
        const char* state = number <= finished ? "done"
                            : number == finished + 1 ? "STUCK"
                            : number <= reached ? "queued"
                                                : "not reached";
        std::snprintf(line, sizeof(line),
                      "EDEN_GPU_HANG_WORK #%u %s %s tick=%" PRIu64 " shader=%016" PRIx64
                      " fragment=%016" PRIx64 " size=%ux%ux%u\n",
                      number, state, KindName(work.kind), work.tick, work.hash,
                      work.fragment_hash, work.size[0], work.size[1], work.size[2]);
        Say(line);
    }
    std::fflush(stderr);
}

} // namespace Vulkan
