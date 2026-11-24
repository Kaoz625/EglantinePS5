// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_4.h>
#endif

#include <cstring>
#include <iterator>
#include <string>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/memory_detect.h"
#include <ranges>
#include "video_core/vulkan_common/unbacked_fallback.h"
#include "video_core/vulkan_common/vma.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "performance.h"
namespace Common { void GuestMemoryUsage(std::size_t* span, std::size_t* held) noexcept; }
extern "C" std::size_t eden_heap_committed(void);
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "common/settings.h"

// The guest's memory gives memory back (EglantinePS5 guest_memory_service.inc; absent elsewhere).
extern "C" [[gnu::weak]] std::size_t eden_memory_recover(std::size_t bytes) noexcept;

namespace Vulkan {
namespace {

// Helpers translating MemoryUsage to flags/usage

    [[maybe_unused]] VkMemoryPropertyFlags MemoryUsagePropertyFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            case MemoryUsage::Upload:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            case MemoryUsage::Download:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            case MemoryUsage::Stream:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    [[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
        if (usage == MemoryUsage::Download) {
            return VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                    : VkMemoryPropertyFlagBits{};
    }

    [[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::Upload:
            case MemoryUsage::Stream:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            case MemoryUsage::Download:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            case MemoryUsage::DeviceLocal:
                return {};
        }
        return {};
    }

    [[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
            case MemoryUsage::Stream:
                return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            case MemoryUsage::Upload:
            case MemoryUsage::Download:
                return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        }
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
} // namespace

//MemoryCommit is now VMA-backed
MemoryCommit::MemoryCommit(VmaAllocator alloc, VmaAllocation a,
                            const VmaAllocationInfo &info) noexcept
        : allocator{alloc}, allocation{a}, memory{info.deviceMemory},
            offset{info.offset}, size{info.size}, mapped_ptr{info.pMappedData} {
    // Log GPU memory allocation
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(memory),
            static_cast<u64>(size),
            0  // Memory property flags (not easily available from VMA)
        );
    }
}

MemoryCommit::~MemoryCommit() { Release(); }

MemoryCommit::MemoryCommit(MemoryCommit &&rhs) noexcept
        : allocator{std::exchange(rhs.allocator, nullptr)},
            allocation{std::exchange(rhs.allocation, nullptr)},
            memory{std::exchange(rhs.memory, VK_NULL_HANDLE)},
            offset{std::exchange(rhs.offset, 0)},
            size{std::exchange(rhs.size, 0)},
            mapped_ptr{std::exchange(rhs.mapped_ptr, nullptr)} {}

MemoryCommit &MemoryCommit::operator=(MemoryCommit &&rhs) noexcept {
    if (this != &rhs) {
        Release();
        allocator = std::exchange(rhs.allocator, nullptr);
        allocation = std::exchange(rhs.allocation, nullptr);
        memory = std::exchange(rhs.memory, VK_NULL_HANDLE);
        offset = std::exchange(rhs.offset, 0);
        size = std::exchange(rhs.size, 0);
        mapped_ptr = std::exchange(rhs.mapped_ptr, nullptr);
    }
    return *this;
}

std::span<u8> MemoryCommit::Map()
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        if (vmaMapMemory(allocator, allocation, &mapped_ptr) != VK_SUCCESS) return {};
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<u8>{static_cast<u8 *>(mapped_ptr), n};
}

std::span<const u8> MemoryCommit::Map() const
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        void *p = nullptr;
        if (vmaMapMemory(allocator, allocation, &p) != VK_SUCCESS) return {};
        const_cast<MemoryCommit *>(this)->mapped_ptr = p;
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<const u8>{static_cast<const u8 *>(mapped_ptr), n};
}

void MemoryCommit::Unmap()
{
    if (allocation && mapped_ptr) {
        vmaUnmapMemory(allocator, allocation);
        mapped_ptr = nullptr;
    }
}

void MemoryCommit::Release() {
    if (allocation && allocator) {
        // Log GPU memory deallocation
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_memory_tracking.GetValue() &&
            memory != VK_NULL_HANDLE) {
            GPU::Logging::GPULogger::GetInstance().LogMemoryDeallocation(
                reinterpret_cast<uintptr_t>(memory)
            );
        }

        if (mapped_ptr) {
            vmaUnmapMemory(allocator, allocation);
            mapped_ptr = nullptr;
        }
        vmaFreeMemory(allocator, allocation);
    }
    allocation = nullptr;
    allocator = nullptr;
    memory = VK_NULL_HANDLE;
    offset = 0;
    size = 0;
}

namespace {
std::mutex vma_probe_mutex;
VmaAllocator vma_probe_allocator{};
void EdenVmaSnapshot(VmaAllocator allocator, unsigned tag) {
    char* json = nullptr;
    vmaBuildStatsString(allocator, &json, VK_TRUE);
    if (!json) return;
    char path[192];
    std::snprintf(path, sizeof(path), "/data/eglantineps5/logs/mkw-vma-%d-%u.json", getpid(), tag);
    const auto bytes = std::strlen(json);
    bool written = false;
    if (bytes <= (8U << 20)) {
        if (FILE* file = std::fopen(path, "w")) {
            written = std::fwrite(json, 1, bytes, file) == bytes;
            std::fclose(file);
        }
    }
    std::fprintf(stderr, "MKW_VMA_MAP tag=%u bytes=%zu written=%u\n", tag, bytes, unsigned(written));
    vmaFreeStatsString(allocator, json);
}
}
extern "C" void eden_vma_probe(unsigned seconds) {
    std::lock_guard lock{vma_probe_mutex};
    if (vma_probe_allocator) EdenVmaSnapshot(vma_probe_allocator, 100000 + seconds);
}
MemoryAllocator::MemoryAllocator(const Device &device_)
        : device{device_}, allocator{device.GetAllocator()},
            properties{device_.GetPhysical().GetMemoryProperties().memoryProperties},
            buffer_image_granularity{
                    device_.GetPhysical().GetProperties().limits.bufferImageGranularity} {

    { std::lock_guard lock{vma_probe_mutex}; vma_probe_allocator = allocator; }
    // Preserve the previous "RenderDoc small heap" trimming behavior that we had in original vma minus the heap bug
    if (device.HasDebuggingToolAttached())
    {
        using namespace Common::Literals;
        ForEachDeviceLocalHostVisibleHeap(device, [this](size_t heap_idx, VkMemoryHeap &heap) {
            if (heap.size <= 256_MiB) {
                for (u32 t = 0; t < properties.memoryTypeCount; ++t) {
                    if (properties.memoryTypes[t].heapIndex == heap_idx) {
                        valid_memory_types &= ~(1u << t);
                    }
                }
            }
        });
    }
}

MemoryAllocator::~MemoryAllocator() {
    std::lock_guard lock{vma_probe_mutex};
    if (vma_probe_allocator == allocator) vma_probe_allocator = nullptr;
    for (auto pool : temporary_pools) if (pool) vmaDestroyPool(allocator, pool);
}

vk::Image MemoryAllocator::CreateImage(const VkImageCreateInfo &ci, bool dedicated) const
{
    vk::Image image;
    vk::Check(AllocateImage(ci, dedicated, false, image));
    return image;
}

vk::Image MemoryAllocator::TryCreateImage(const VkImageCreateInfo &ci, bool dedicated) const {
    vk::Image image;
    const VkResult result = AllocateImage(ci, dedicated, true, image);
    if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_OUT_OF_HOST_MEMORY) {
        vk::Check(result);
    }
    return image;
}

// H49-H51 diag (EDEN_SPARSE_POOL / EDEN_VMA_POOL lines, every 10 s while memory is being
// allocated): every separate pool's blocks against what is allocated in them, and per memory
// type the default blocks (images and dedicated memory) = all minus the separate pools.
// MKW PHAT5 H50 U1 (24 Nov): with the sparse tiles packed, RADV's device-local BOs stayed at
// ~3.0 GB as on H49c (16 MiB blocks 96 -> 72, plus 188 x 2 MiB): the memory the sparse pools
// gave back was taken by the default 16 MiB blocks (+592 MiB). This shows how full those are.
static void LogVmaPools(const Device& device, VmaAllocator allocator) {
    static std::atomic<long long> pool_logged{0};
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    long long last = pool_logged.load(std::memory_order_relaxed);
    if (now - last < 10 ||
        !pool_logged.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    std::array<VmaStatistics, VK_MAX_MEMORY_TYPES> separate{};
    for (const auto [pool_kind, name] :
         {std::pair{Device::SeparatePool::SparsePages, "pages"},
          std::pair{Device::SeparatePool::SparseTiles, "tiles"},
          std::pair{Device::SeparatePool::DeviceBuffers, "buffers"},
          std::pair{Device::SeparatePool::SmallImages, "small_images"}}) {
        for (u32 type = 0; type < VK_MAX_MEMORY_TYPES; ++type) {
            if (!device.HasSeparatePool(pool_kind, type)) {
                continue;
            }
            VmaStatistics statistics{};
            vmaGetPoolStatistics(allocator, device.GetSeparatePool(pool_kind, type), &statistics);
            separate[type].blockCount += statistics.blockCount;
            separate[type].allocationCount += statistics.allocationCount;
            separate[type].blockBytes += statistics.blockBytes;
            separate[type].allocationBytes += statistics.allocationBytes;
            std::fprintf(stderr,
                         "%s %s type=%u blocks=%u block_mib=%llu used_mib=%llu allocations=%u\n",
                         pool_kind == Device::SeparatePool::DeviceBuffers ||
                                 pool_kind == Device::SeparatePool::SmallImages
                             ? "EDEN_VMA_POOL"
                             : "EDEN_SPARSE_POOL",
                         name, type, statistics.blockCount,
                         static_cast<unsigned long long>(statistics.blockBytes >> 20),
                         static_cast<unsigned long long>(statistics.allocationBytes >> 20),
                         statistics.allocationCount);
        }
    }
    // Default blocks = the memory type's totals minus its separate pools (a dedicated allocation
    // counts as a block of its own there). unused_ranges / largest_unused cover the whole type.
    VmaTotalStatistics total{};
    vmaCalculateStatistics(allocator, &total);
    for (u32 type = 0; type < VK_MAX_MEMORY_TYPES; ++type) {
        const VmaStatistics& all = total.memoryType[type].statistics;
        if (all.blockCount <= separate[type].blockCount) {
            continue;
        }
        const auto& detail = total.memoryType[type];
        std::fprintf(stderr,
                     "EDEN_VMA_POOL default type=%u blocks=%u block_mib=%llu used_mib=%llu "
                     "allocations=%u unused_ranges=%u largest_unused_kib=%llu\n",
                     type, all.blockCount - separate[type].blockCount,
                     static_cast<unsigned long long>(
                         (all.blockBytes - separate[type].blockBytes) >> 20),
                     static_cast<unsigned long long>(
                         (all.allocationBytes - separate[type].allocationBytes) >> 20),
                     all.allocationCount - separate[type].allocationCount, detail.unusedRangeCount,
                     static_cast<unsigned long long>(
                         detail.unusedRangeCount != 0 ? detail.unusedRangeSizeMax >> 10 : 0));
    }
}

VkResult MemoryAllocator::AllocateImage(const VkImageCreateInfo &ci, bool dedicated, bool may_fail,
                                        vk::Image &image) const {
    LogVmaPools(device, allocator);
    const VmaAllocationCreateInfo alloc_ci = {
            // Best fit: images freed by the thousand leave holes in the blocks, which new ones
            // should fill before VMA takes another block.
            .flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT |
                                                           VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT) |
                     (dedicated ? VmaAllocationCreateFlags{VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT}
                               : VmaAllocationCreateFlags{0}),
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .requiredFlags = 0,
            .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            .memoryTypeBits = 0,
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
            .priority = 0.f,
    };

    VkImage handle{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info{};
    const u64 texels = u64{ci.extent.width} * ci.extent.height * ci.extent.depth * ci.arrayLayers;
    u64 bytes = 0;
    if (texels > MaxBackedImageTexels) {
        // Its memory size decides: an image the driver won't create is too large to back.
        const auto& dld = device.GetDispatchLoader();
        VkImage probe{};
        bytes = ~u64{0};
        if (dld.vkCreateImage(*device.GetLogical(), &ci, nullptr, &probe) == VK_SUCCESS) {
            bytes = device.GetLogical().GetImageMemoryRequirements(probe).size;
            dld.vkDestroyImage(*device.GetLogical(), probe, nullptr);
        }
    }
    const bool huge = IsTooLargeToBack(texels, bytes);
    VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bool small_pooled = false;
    // H52: small images in a pool of small blocks of their own, lowest offset first (as the
    // sparse tiles, H50). MKW PHAT5 H51 V1 (24 Nov), race 1: the default device-local blocks held
    // 2.1-2.3 GB for 1.8-2.1 GB of allocations (130-330 MiB of holes, ~900 unused ranges), and
    // the collector deletes thousands of small images a minute; their holes are refilled, but a
    // 16 MiB block seldom empties to go back to the pool the guest's memory comes from.
    if (!huge && !dedicated && Device::SmallImageTexels() != 0 &&
        texels <= Device::SmallImageTexels()) {
        static std::atomic<s32> small_type{-1};
        s32 type = small_type.load(std::memory_order_relaxed);
        if (type < 0) {
            u32 found{};
            if (vmaFindMemoryTypeIndexForImageInfo(allocator, &ci, &alloc_ci, &found) ==
                VK_SUCCESS) {
                type = static_cast<s32>(found);
                small_type.store(type, std::memory_order_relaxed);
            }
        }
        if (type >= 0) {
            if (const VmaPool pool = device.GetSeparatePool(Device::SeparatePool::SmallImages,
                                                            static_cast<u32>(type))) {
                VmaAllocationCreateInfo pool_ci = alloc_ci;
                pool_ci.pool = pool;
                pool_ci.flags = (pool_ci.flags & ~static_cast<VmaAllocationCreateFlags>(
                                                     VMA_ALLOCATION_CREATE_STRATEGY_MASK)) |
                                static_cast<VmaAllocationCreateFlags>(
                                    VMA_ALLOCATION_CREATE_STRATEGY_MIN_OFFSET_BIT);
                result = vmaCreateImage(allocator, &ci, &pool_ci, &handle, &allocation,
                                        &alloc_info);
                small_pooled = result == VK_SUCCESS;
                if (!small_pooled) {
                    handle = VkImage{};
                    allocation = VmaAllocation{};
                }
            }
        }
    }
    if (!small_pooled) {
        result = huge ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                      : vmaCreateImage(allocator, &ci, &alloc_ci, &handle, &allocation,
                                       &alloc_info);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        // Over the device memory budget (Switch 2 titles fill 8 GB cards): let the driver page
        // it out rather than failing the draw and aborting.
        static u32 logged{};
        if (logged++ < 32) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) is over the memory "
                        "budget; allocating it anyway",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
        }
        pressure_events.fetch_add(1, std::memory_order_relaxed);
        VmaAllocationCreateInfo relaxed = alloc_ci;
        relaxed.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &ci, &relaxed, &handle, &allocation, &alloc_info);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        pressure_events.fetch_add(1, std::memory_order_relaxed);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && ci.imageType != VK_IMAGE_TYPE_1D &&
        UsesUnbackedFallback(texels, bytes, may_fail)) {
        // Too big to back at all, e.g. title 1's sparse virtual texture arrays (8192x8192,
        // 68 layers, ~23 GB): on the console only resident tiles have memory. Use an image with
        // nothing bound, so views and copies still work, instead of aborting. A smaller image
        // gets one only for a caller that would throw otherwise.
        if (vk::Image sparse = CreateUnbackedImage(ci)) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) does not fit; using an "
                        "unbacked sparse image",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
            image = std::move(sparse);
            return VK_SUCCESS;
        }
    }
    if (result != VK_SUCCESS) {
        failed_images.fetch_add(1, std::memory_order_relaxed);
        static u32 failed_logged{};
        if (failed_logged == 0) {
            // What ran out: the device's heaps or the host's commit. Before the error line,
            // which is the one that reaches the log file when the session ends here.
            LogMemoryTrace(device);
        }
        if (failed_logged++ < 32) {
            LOG_ERROR(Render_Vulkan, "Image {}x{}x{} ({} levels, {} layers, format {}) failed: {}",
                      ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                      ci.arrayLayers, static_cast<u32>(ci.format), static_cast<int>(result));
        }
        return result;
    }

    NoteHostPlacement(allocation, alloc_info.size, true);

    // Log GPU memory allocation for images
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );
    }

    image = vk::Image(handle, ci.usage, *device.GetLogical(), allocator, allocation,
                      device.GetDispatchLoader());
    return VK_SUCCESS;
}

void MemoryAllocator::NoteHostPlacement(VmaAllocation allocation, VkDeviceSize size,
                                        bool is_image) const {
    // With the device heap over its budget, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE does not fail:
    // it takes a memory type of the host heap instead, so none of the out-of-memory paths above
    // runs and the texture cache never hears of it. RTX 4050 Laptop (6 GB, ~4.75 GB budget) in
    // title 1's Castle Town: 288-424 MiB of images and buffers ended up in a host type with no
    // property flags and the frame rate halved (50-57 -> 20-26 fps), with "out of memory 0 MiB"
    // all along. On an integrated GPU every heap is the same memory.
    if (allocation == VK_NULL_HANDLE || device.IsIntegrated()) {
        return;
    }
    VkMemoryPropertyFlags flags{};
    vmaGetAllocationMemoryProperties(allocator, allocation, &flags);
    if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
        return;
    }
    const u64 total = spilled_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    // Not a pressure event: each of those keeps 512 MiB more free, and counting spills as such
    // (one every 2 s) took the margin to 1.5 GiB while a scene loaded on that laptop and kept
    // the cache under the scene's working set for minutes (ASTC decodes 55/s -> 950/s). The
    // texture cache takes the bytes and sizes its answer to them (TextureCache::TickFrame).
    spill_pending.fetch_add(size, std::memory_order_relaxed);
    // The first ones, then at most one every 2 s.
    constexpr s64 Interval = std::chrono::nanoseconds{std::chrono::seconds{2}}.count();
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    bool log = spill_logged.fetch_add(1, std::memory_order_relaxed) < 32;
    if (!log) {
        s64 last = last_spill_log.load(std::memory_order_relaxed);
        log = now - last >= Interval &&
              last_spill_log.compare_exchange_strong(last, now, std::memory_order_relaxed);
    }
    if (log) {
        LOG_WARNING(Render_Vulkan,
                    "Device-local {} of {} KiB placed in host memory (device heap over budget; "
                    "{} MiB so far)",
                    is_image ? "image" : "buffer", size >> 10, total >> 20);
    }
}

vk::Image MemoryAllocator::CreateUnbackedImage(const VkImageCreateInfo &ci, bool aliased) const {
    if (ci.imageType == VK_IMAGE_TYPE_1D) {
        return vk::Image{};
    }
    VkImageCreateInfo sparse_ci = ci;
    sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (aliased) {
        sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    }
    VkImage sparse{};
    VkResult sparse_result = device.GetDispatchLoader().vkCreateImage(*device.GetLogical(), &sparse_ci, nullptr, &sparse);
    if (sparse_result != VK_SUCCESS) {
        VkImageCreateInfo binding_only = ci;
        binding_only.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
        const VkResult second = device.GetDispatchLoader().vkCreateImage(*device.GetLogical(), &binding_only, nullptr, &sparse);
        std::fprintf(stderr, "EDEN_SPARSE_IMAGE %ux%ux%u, %u layers, %u levels, format %d, usage 0x%x, flags 0x%x, tiling %d: "
                     "with residency %d, binding alone %d\n",
                     ci.extent.width, ci.extent.height, ci.extent.depth, ci.arrayLayers, ci.mipLevels, int(ci.format),
                     unsigned(ci.usage), unsigned(ci.flags), int(ci.tiling), int(sparse_result), int(second));
        if (second != VK_SUCCESS) {
            // Once: which smaller sizes does the driver accept? Each probe is made and destroyed at once.
            static std::atomic<bool> probed{false};
            if (!probed.exchange(true)) {
                for (u32 layers = ci.arrayLayers / 2; layers >= 1; layers /= 2) {
                    VkImageCreateInfo probe = sparse_ci;
                    probe.arrayLayers = layers;
                    VkImage probe_image{};
                    const VkResult probed_result =
                        device.GetDispatchLoader().vkCreateImage(*device.GetLogical(), &probe, nullptr, &probe_image);
                    std::fprintf(stderr, "EDEN_SPARSE_PROBE %u layers: %d\n", layers, int(probed_result));
                    if (probed_result == VK_SUCCESS) {
                        device.GetDispatchLoader().vkDestroyImage(*device.GetLogical(), probe_image, nullptr);
                    }
                    if (layers == 1) break;
                }
            }
            return vk::Image{};
        }
    }
    return vk::Image(sparse, ci.usage, *device.GetLogical(), allocator, VK_NULL_HANDLE,
                     device.GetDispatchLoader());
}

VmaAllocation MemoryAllocator::AllocateSparseMemory(const VkMemoryRequirements &requirements,
                                                   VkDeviceMemory &memory,
                                                   VkDeviceSize &offset) const {
    const VmaAllocationCreateInfo alloc_ci{
        .flags = 0,
        .usage = VMA_MEMORY_USAGE_UNKNOWN,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .preferredFlags = 0,
        .memoryTypeBits = requirements.memoryTypeBits,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    // In a pool of their own (see Device::SeparatePool); anything over half a block, or a pool
    // allocation that fails, takes the default blocks.
    u32 memory_type{};
    if (requirements.size <= Device::SeparatePoolBlockSize / 2 &&
        vmaFindMemoryTypeIndex(allocator, requirements.memoryTypeBits, &alloc_ci, &memory_type) ==
            VK_SUCCESS) {
        // H50: a single tile (64 KiB) goes to the SparseTiles pool (2 MiB blocks, lowest offset
        // first); mip tails, layers and zero blocks stay in the SparsePages pool (16 MiB blocks).
        // MKW PHAT5 H49c T1: one sparse-page pool held 544-576 MiB of blocks with 136-177 MiB
        // allocated in races 2-3 (scattered 64 KiB tiles kept every 16 MiB block alive), with the
        // direct-memory pool's largest free piece at 1-4 MiB.
        const VkDeviceSize tile_block = Device::SparseTileBlockSize();
        const bool tile = tile_block != 0 && requirements.size <= (VkDeviceSize{64} << 10);
        const Device::SeparatePool kind =
            tile ? Device::SeparatePool::SparseTiles : Device::SeparatePool::SparsePages;
        if (const VmaPool pool = device.GetSeparatePool(kind, memory_type)) {
            VmaAllocationCreateInfo pool_ci = alloc_ci;
            pool_ci.pool = pool;
            if (tile) {
                pool_ci.flags |= VMA_ALLOCATION_CREATE_STRATEGY_MIN_OFFSET_BIT;
            }
            if (vmaAllocateMemory(allocator, &requirements, &pool_ci, &allocation, &info) ==
                VK_SUCCESS) {
                memory = info.deviceMemory;
                offset = info.offset;
                LogVmaPools(device, allocator);
                return allocation;
            }
        }
    }
    if (vmaAllocateMemory(allocator, &requirements, &alloc_ci, &allocation, &info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    memory = info.deviceMemory;
    offset = info.offset;
    return allocation;
}

void MemoryAllocator::FreeSparseMemory(VmaAllocation allocation) const {
    if (allocation != VK_NULL_HANDLE) {
        vmaFreeMemory(allocator, allocation);
    }
}

bool MemoryAllocator::CreateBufferInSeparatePool(const VkBufferCreateInfo& ci, MemoryUsage usage,
                                                 const VmaAllocationCreateInfo& alloc_ci,
                                                 VkBuffer& handle, VmaAllocation& allocation,
                                                 VmaAllocationInfo& alloc_info) const {
    // The buffer cache's buffers, churned by its collector: in a pool of their own (see
    // Device::SeparatePool). Anything over half a block takes the default blocks (dedicated).
    if (usage != MemoryUsage::DeviceLocal || ci.size > Device::SeparatePoolBlockSize / 2) {
        return false;
    }
    u32 memory_type{};
    {
        std::scoped_lock lk{buffer_type_mutex};
        const auto key = (u64{ci.usage} << 32) | ci.flags;
        if (const auto it = buffer_memory_types.find(key); it != buffer_memory_types.end()) {
            memory_type = it->second;
        } else if (vmaFindMemoryTypeIndexForBufferInfo(allocator, &ci, &alloc_ci, &memory_type) ==
                   VK_SUCCESS) {
            buffer_memory_types.emplace(key, memory_type);
        } else {
            return false;
        }
    }
    const VmaPool pool = device.GetSeparatePool(Device::SeparatePool::DeviceBuffers, memory_type);
    if (pool == VK_NULL_HANDLE) {
        return false;
    }
    VmaAllocationCreateInfo pool_ci = alloc_ci;
    pool_ci.pool = pool;
    pool_ci.memoryTypeBits = 0;
    // H52: lowest offset first, as the sparse tiles (H50), so the pool's last blocks drain and go
    // back to the driver. MKW PHAT5 H51 V1 (24 Nov), race 1: the buffer pool held 256-352 MiB of
    // 16 MiB blocks for 99-172 MiB of buffers. EDEN_VMA_BUFFERS_MIN_OFFSET=0 = H51.
    static const bool buffers_min_offset = [] {
        const char* value = std::getenv("EDEN_VMA_BUFFERS_MIN_OFFSET");
        return value == nullptr || value[0] != '0';
    }();
    if (buffers_min_offset) {
        pool_ci.flags = (pool_ci.flags & ~static_cast<VmaAllocationCreateFlags>(
                                             VMA_ALLOCATION_CREATE_STRATEGY_MASK)) |
                        static_cast<VmaAllocationCreateFlags>(
                            VMA_ALLOCATION_CREATE_STRATEGY_MIN_OFFSET_BIT);
    }
    return vmaCreateBuffer(allocator, &ci, &pool_ci, &handle, &allocation, &alloc_info) ==
           VK_SUCCESS;
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const {
    vk::Buffer buffer;
    vk::Check(AllocateBuffer(ci, usage, 0, buffer));
    return buffer;
}

vk::Buffer MemoryAllocator::TryCreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const {
    vk::Buffer buffer;
    const VkResult result = AllocateBuffer(ci, usage, 0, buffer);
    if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_OUT_OF_HOST_MEMORY) {
        vk::Check(result);
    }
    return buffer;
}

VkResult MemoryAllocator::AllocateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                         VkDeviceSize min_alignment, vk::Buffer &buffer) const {
    // MESA will do memcpy() if not marked as host cached, so just force mark it for most buffers
    auto const anv_flags = (usage == MemoryUsage::Stream
        && device.GetDriverID() == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA)
        ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage) | anv_flags,
        .memoryTypeBits = usage == MemoryUsage::Stream ? 0u : valid_memory_types,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };

    VkBuffer handle{};
    VmaAllocationInfo alloc_info{};
    VmaAllocation allocation{};
    VkMemoryPropertyFlags property_flags{};

    const bool aligned = min_alignment > 1;
    const auto create = [&](const VmaAllocationCreateInfo& create_ci) {
        return aligned ? vmaCreateBufferWithAlignment(allocator, &ci, &create_ci, min_alignment,
                                                      &handle, &allocation, &alloc_info)
                       : vmaCreateBuffer(allocator, &ci, &create_ci, &handle, &allocation,
                                         &alloc_info);
    };
    if (aligned ||
        !CreateBufferInSeparatePool(ci, usage, alloc_ci, handle, allocation, alloc_info)) {
        VkResult result = create(alloc_ci);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY) {
            // Over the device memory budget, as images are in CreateImage: buffers have no
            // unbacked fallback, and the vk::Exception a failed staging or buffer cache
            // allocation throws from the GPU thread aborts the process. Phones share RAM with
            // the GPU, so the budget is often lower than the memory that is really free.
            static u32 logged{};
            if (logged++ < 32) {
                LOG_WARNING(Render_Vulkan,
                            "Buffer of {} bytes (usage {}) is over the memory budget; "
                            "allocating it anyway",
                            ci.size, static_cast<u32>(usage));
            }
            // The texture cache answers each pressure event by keeping more device memory free.
            // Upload and download buffers live in host memory on a discrete GPU, so they only
            // count when the heaps are shared or the buffer asked for device-local memory.
            if (device.IsIntegrated() || usage == MemoryUsage::DeviceLocal ||
                usage == MemoryUsage::Stream) {
                pressure_events.fetch_add(1, std::memory_order_relaxed);
            }
            VmaAllocationCreateInfo relaxed = alloc_ci;
            relaxed.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            result = create(relaxed);
            // EglantinePS5 H39: a refused buffer was fatal (the GPU thread aborts on the
            // exception): MKW race load, VK OOM 525-587 s (H37 K1/K2) with recoveries that had
            // just packed what they were asked. Ask the guest's memory for more, growing, and try
            // again a few times; a stall instead of the end. EDEN_GRAPHICS_RETRY=0 = H38.
            static const bool retry_on = [] {
                const char* value = std::getenv("EDEN_GRAPHICS_RETRY");
                return value == nullptr || value[0] != '0';
            }();
            for (unsigned attempt = 0; retry_on && result != VK_SUCCESS && attempt < 6 &&
                                       &eden_memory_recover != nullptr;
                 ++attempt) {
                const std::size_t want = (static_cast<std::size_t>(ci.size) + (std::size_t{64} << 20))
                                         << (attempt < 3 ? attempt : 3);
                const std::size_t got = eden_memory_recover(want);
                result = create(relaxed);
                static std::atomic<u32> retry_lines{};
                if (retry_lines.fetch_add(1, std::memory_order_relaxed) < 64) {
                    std::fprintf(stderr, "EDEN_GRAPHICS_RETRY buffer=%lluKiB attempt=%u asked=%zuMiB got=%zuMiB result=%d\n",
                                 static_cast<unsigned long long>(ci.size >> 10), attempt, want >> 20, got >> 20,
                                 int(result));
                }
            }
            if (result != VK_SUCCESS) {
                // EglantinePS5: what the pool and the guest held when even the allocation
                // outside the budget was refused.
                VmaBudget held[VK_MAX_MEMORY_HEAPS]{};
                vmaGetHeapBudgets(allocator, held);
                unsigned long long blocks = 0, used = 0;
                for (const VmaBudget& heap : held) {
                    blocks += heap.statistics.blockBytes;
                    used += heap.statistics.allocationBytes;
                }
                const auto free_block = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed);
                std::size_t guest_span = 0, guest_held = 0;
                ::Common::GuestMemoryUsage(&guest_span, &guest_held);
                std::fprintf(stderr, "EDEN_GRAPHICS_MEMORY refused outside the budget too (result %d): the pool's largest free "
                             "block is %llu MiB; the allocator holds %llu MiB in blocks, %llu MiB of it in use; the guest's "
                             "memory holds %llu MiB, the heap %llu MiB\n",
                             int(result), free_block ? free_block() >> 20 : 0ull, blocks >> 20, used >> 20,
                             static_cast<unsigned long long>(guest_held >> 20),
                             static_cast<unsigned long long>(eden_heap_committed() >> 20));
                ::Eden::Performance::ReportHeap("a buffer was refused");
            }
        }
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);
    if (usage == MemoryUsage::DeviceLocal) {
        NoteHostPlacement(allocation, alloc_info.size, false);
    }

    // Log GPU memory allocation for buffers
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            property_flags
        );
    }

    u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
    const std::span<u8> mapped_data = data ? std::span<u8>{data, ci.size} : std::span<u8>{};
    const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::MemoryLocation location{
        .memory = alloc_info.deviceMemory,
        .offset = alloc_info.offset,
        .memory_type = alloc_info.memoryType,
    };
    buffer = vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data,
                        is_coherent, location, device.GetDispatchLoader());
    return VK_SUCCESS;
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                         VkDeviceSize min_alignment) const {
    vk::Buffer buffer;
    vk::Check(AllocateBuffer(ci, usage, min_alignment, buffer));
    return buffer;
}

MemoryCommit MemoryAllocator::Commit(const VkMemoryRequirements &reqs, MemoryUsage usage)
{
    const auto vma_usage = MemoryUsageVma(usage);
    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.memoryTypeBits = reqs.memoryTypeBits & valid_memory_types;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);

    VmaAllocation a{};
    VmaAllocationInfo info{};

    VkResult res = vmaAllocateMemory(allocator, &reqs, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        // Relax 1: drop budget constraint
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemory(allocator, &reqs, &ci2, &a, &info);

        // Relax 2: if we preferred DEVICE_LOCAL, drop that preference
        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemory(allocator, &reqs, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    return MemoryCommit(allocator, a, info);
}

MemoryCommit MemoryAllocator::Commit(const vk::Buffer &buffer, MemoryUsage usage) {
    // Allocate memory appropriate for this buffer automatically
    const auto vma_usage = MemoryUsageVma(usage);

    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);
    ci.pool = VK_NULL_HANDLE;
    ci.pUserData = nullptr;
    ci.priority = 0.0f;

    const VkBuffer raw = *buffer;

    VmaAllocation a{};
    VmaAllocationInfo info{};

    // Let VMA infer memory requirements from the buffer
    VkResult res = vmaAllocateMemoryForBuffer(allocator, raw, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemoryForBuffer(allocator, raw, &ci2, &a, &info);

        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemoryForBuffer(allocator, raw, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    vk::Check(vmaBindBufferMemory2(allocator, a, 0, raw, nullptr));
    return MemoryCommit(allocator, a, info);
}

#ifdef _WIN32
namespace {
/// The DXGI adapter of the Vulkan device (matched by LUID), or null. Looked up again when the
/// device changes (another GPU picked between sessions).
IDXGIAdapter3* FindDxgiAdapter(const Device& device) {
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    device.GetPhysical().GetProperties2(properties);
    if (!id.deviceLUIDValid) {
        return nullptr;
    }
    LUID luid{};
    std::memcpy(&luid, id.deviceLUID, sizeof(luid));
    static LUID cached_luid{};
    static IDXGIAdapter3* cached{};
    if (cached != nullptr && cached_luid.LowPart == luid.LowPart &&
        cached_luid.HighPart == luid.HighPart) {
        return cached;
    }
    if (cached != nullptr) {
        cached->Release();
        cached = nullptr;
    }
    cached_luid = luid;
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) {
        return nullptr;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory4* factory{};
    if (create == nullptr ||
        FAILED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    if (FAILED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3),
                                          reinterpret_cast<void**>(&cached)))) {
        cached = nullptr;
    }
    factory->Release();
    return cached;
}
} // Anonymous namespace
#endif

void LogMemoryTrace(const Device& device) {
    const Common::ProcessMemoryUsage process = Common::GetProcessMemoryUsage();
    LOG_INFO(Render_Vulkan,
             "Memory trace: process commit {} MiB, working set {} MiB (private {} MiB), system "
             "commit {} of {} MiB",
             process.commit >> 20, process.working_set >> 20, process.private_working_set >> 20,
             process.system_commit >> 20, process.system_commit_limit >> 20);
#ifdef _WIN32
    // This process's use of the card's memory (local) and of system memory for the GPU
    // (non-local: host memory types, and what the OS moved out of video memory).
    IDXGIAdapter3* const adapter = FindDxgiAdapter(device);
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO non_local{};
    if (adapter != nullptr &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
        SUCCEEDED(
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local))) {
        LOG_INFO(Render_Vulkan,
                 "Memory trace: DXGI local {} of {} MiB, non-local {} of {} MiB",
                 local.CurrentUsage >> 20, local.Budget >> 20, non_local.CurrentUsage >> 20,
                 non_local.Budget >> 20);
    }
#endif
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    std::string heaps;
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        const VmaBudget& budget = budgets[heap];
        fmt::format_to(std::back_inserter(heaps), "{}{} ({}): driver {} of {} MiB, VMA {}/{} MiB",
                       heap == 0 ? "" : "; ", heap,
                       (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0
                           ? "local"
                           : "host",
                       budget.usage >> 20, budget.budget >> 20,
                       budget.statistics.allocationBytes >> 20,
                       budget.statistics.blockBytes >> 20);
    }
    LOG_INFO(Render_Vulkan, "Memory trace: heaps {}", heaps);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    std::string types;
    for (u32 type = 0; type < properties->memoryTypeCount; ++type) {
        const VmaStatistics& s = stats.memoryType[type].statistics;
        if (s.blockCount == 0) {
            continue;
        }
        fmt::format_to(std::back_inserter(types), "{}{} (heap {}, flags {:#x}): {} allocs {}/{} MiB",
                       types.empty() ? "" : "; ", type, properties->memoryTypes[type].heapIndex,
                       static_cast<u32>(properties->memoryTypes[type].propertyFlags),
                       s.allocationCount, s.allocationBytes >> 20, s.blockBytes >> 20);
    }
    LOG_INFO(Render_Vulkan, "Memory trace: types {}", types);
}

void LogMemoryStatistics(const Device& device) {
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    for (u32 type = 0; type < properties->memoryTypeCount; ++type) {
        const VmaStatistics& s = stats.memoryType[type].statistics;
        if (s.blockCount == 0) {
            continue;
        }
        LOG_DEBUG(Render_Vulkan, "VMA type {} (heap {}, flags {:#x}): {} blocks {} MiB, {} allocations {} MiB",
                 type, properties->memoryTypes[type].heapIndex,
                 static_cast<u32>(properties->memoryTypes[type].propertyFlags), s.blockCount,
                 s.blockBytes >> 20, s.allocationCount, s.allocationBytes >> 20);
    }
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        LOG_INFO(Render_Vulkan, "VMA heap {}: usage {} MiB of budget {} MiB, VMA blocks {} MiB",
                 heap, budgets[heap].usage >> 20, budgets[heap].budget >> 20,
                 budgets[heap].statistics.blockBytes >> 20);
    }
}

} // namespace Vulkan
