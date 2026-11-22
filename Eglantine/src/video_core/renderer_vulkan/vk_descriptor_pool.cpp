// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"
#include <ranges>
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

#if defined(__PROSPERO__) // PS5
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
// The app heap (headless/heap_arenas.inc): gives its empty arenas and spaces back; the bytes.
extern "C" std::size_t eden_heap_trim(void) __attribute__((weak));
#endif

namespace Vulkan {

// Prefer small grow rates to avoid saturating the descriptor pool with barely used pipelines
constexpr size_t SETS_GROW_RATE = 16;
constexpr s32 SCORE_THRESHOLD = 3;

struct DescriptorBank {
    DescriptorBankInfo info;
    std::vector<vk::DescriptorPool> pools;
};

bool DescriptorBankInfo::IsSuperset(const DescriptorBankInfo& subset) const noexcept {
    return uniform_buffers >= subset.uniform_buffers && storage_buffers >= subset.storage_buffers &&
           texture_buffers >= subset.texture_buffers && image_buffers >= subset.image_buffers &&
           textures >= subset.textures && images >= subset.images &&
           sampled_images >= subset.sampled_images && samplers >= subset.samplers &&
           update_after_bind == subset.update_after_bind;
}

template <typename Descriptors>
static u32 Accumulate(const Descriptors& descriptors) {
    u32 count = 0;
    for (const auto& descriptor : descriptors) {
        count += descriptor.count;
    }
    return count;
}

static DescriptorBankInfo MakeBankInfo(std::span<const Shader::Info> infos, bool update_after_bind) {
    DescriptorBankInfo bank{.update_after_bind = update_after_bind};
    for (const Shader::Info& info : infos) {
        bank.uniform_buffers += Accumulate(info.constant_buffer_descriptors);
        bank.storage_buffers += Accumulate(info.storage_buffers_descriptors);
        bank.texture_buffers += Accumulate(info.texture_buffer_descriptors);
        bank.image_buffers += Accumulate(info.image_buffer_descriptors);
        bank.textures += Accumulate(info.texture_descriptors);
        bank.images += Accumulate(info.image_descriptors);
    }
    bank.score = bank.uniform_buffers + bank.storage_buffers + bank.texture_buffers +
                 bank.image_buffers + bank.textures + bank.images;
    return bank;
}

#if defined(__PROSPERO__) // PS5
// Kirby Air Riders (21 Nov 2026): the driver's descriptor and upload memory (RADV priority 30) was
// 860 MiB in 91 buffers before the first race and 1,032 MiB at the out-of-memory, in the 12 GiB
// pool the CPU shares with the GPU. Every bank made a pool of 96 sets at once (sets per pool on
// AMD), whether or not a draw ever used it, and a Switch 2 shader's set can hold many descriptors.
// A pool is now sized in bytes: as many sets as fit EDEN_DESCRIPTOR_POOL_KIB (default 512), at
// least one batch (SETS_GROW_RATE) and at most the device's count, and a bank's first pool is made
// at its first sets, not with the bank. EDEN_DESCRIPTOR_POOL_KIB=0: as before (96 sets, at once).
static std::size_t PoolBudgetBytes() {
    static const std::size_t budget = [] {
        const char* value = std::getenv("EDEN_DESCRIPTOR_POOL_KIB");
        const long kib = value != nullptr ? std::strtol(value, nullptr, 10) : 512;
        return kib > 0 ? std::size_t(kib) << 10 : std::size_t{0};
    }();
    return budget;
}
// RADV's bytes for one set of the bank (radv_descriptor_pool.c sizes; images at their largest).
static std::size_t SetBytes(const DescriptorBankInfo& info) {
    return 16 * std::size_t(info.uniform_buffers + info.storage_buffers + info.texture_buffers +
                            info.image_buffers + info.samplers) +
           32 * std::size_t(info.images) + 64 * std::size_t(info.sampled_images) +
           96 * std::size_t(info.textures);
}
static std::atomic<u64> pools_made{0}, pool_bytes_made{0};
// Kirby Air Riders (22 Nov 2026): a Switch 2 stage binds ~2,050 textures, so one set is ~197 KiB
// (~394 KiB with two stages), and every pipeline's allocator took its sets 16 at a time: ~3-6 MiB
// per pipeline, never given back. Descriptor pools (RADV priority 30) grew from 170 MiB to
// 0.8-1.05 GiB while the race loaded its pipelines (256 pools, one per 16-set batch) and the app
// ran out of memory. A set of EDEN_DESCRIPTOR_BIG_SET_KIB (default 32) or more is now taken
// EDEN_DESCRIPTOR_BIG_SETS (default 2) at a time; a pipeline that draws more grows by that much
// again. EDEN_DESCRIPTOR_BIG_SETS=0 (or 16): as before.
static std::size_t BatchSets(const DescriptorBankInfo& info) {
    static const std::size_t big_bytes = [] {
        const char* value = std::getenv("EDEN_DESCRIPTOR_BIG_SET_KIB");
        const long kib = value != nullptr ? std::strtol(value, nullptr, 10) : 32;
        return std::size_t(kib > 0 ? kib : 32) << 10;
    }();
    static const std::size_t big_sets = [] {
        const char* value = std::getenv("EDEN_DESCRIPTOR_BIG_SETS");
        const long sets = value != nullptr ? std::strtol(value, nullptr, 10) : 2;
        return sets > 0 && sets < long(SETS_GROW_RATE) ? std::size_t(sets) : SETS_GROW_RATE;
    }();
    return SetBytes(info) >= big_bytes ? big_sets : SETS_GROW_RATE;
}
#endif

static void AllocatePool(const Device& device, DescriptorBank& bank) {
    std::array<VkDescriptorPoolSize, 8> pool_sizes;
    size_t pool_cursor{};
#if defined(__PROSPERO__) // PS5
    const u32 device_sets = device.GetSetsPerPool();
    const std::size_t set_bytes = std::max<std::size_t>(SetBytes(bank.info), 16);
    const std::size_t budget = PoolBudgetBytes();
    const std::size_t batch = BatchSets(bank.info);
    const u32 sets_per_pool =
        budget == 0 ? device_sets
                    : u32(std::clamp<std::size_t>(budget / set_bytes, batch,
                                                  std::max<std::size_t>(device_sets, batch)));
    {
        const u64 made = pools_made.fetch_add(1, std::memory_order_relaxed) + 1;
        const u64 bytes = pool_bytes_made.fetch_add(u64(set_bytes) * sets_per_pool,
                                                    std::memory_order_relaxed) +
                          u64(set_bytes) * sets_per_pool;
        if (made <= 64 || made % 32 == 0) {
            std::fprintf(stderr,
                         "EDEN_DESCRIPTOR_POOL made=%llu sets=%u set_bytes=%zu pool_kib=%zu "
                         "total_mib=%llu (ub=%u sb=%u tb=%u ib=%u tex=%u img=%u si=%u smp=%u uab=%d)\n",
                         (unsigned long long)made, sets_per_pool, set_bytes,
                         (set_bytes * sets_per_pool) >> 10, (unsigned long long)(bytes >> 20),
                         bank.info.uniform_buffers, bank.info.storage_buffers,
                         bank.info.texture_buffers, bank.info.image_buffers, bank.info.textures,
                         bank.info.images, bank.info.sampled_images, bank.info.samplers,
                         int(bank.info.update_after_bind));
        }
    }
#else
    const u32 sets_per_pool = device.GetSetsPerPool();
#endif
    const auto add = [&](VkDescriptorType type, u32 count) {
        if (count > 0) {
            pool_sizes[pool_cursor++] = {
                .type = type,
                .descriptorCount = count * sets_per_pool,
            };
        }
    };
    const auto& info{bank.info};
    add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, info.uniform_buffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, info.storage_buffers);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, info.texture_buffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, info.image_buffers);
    add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info.textures);
    add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, info.images);
    add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, info.sampled_images);
    add(VK_DESCRIPTOR_TYPE_SAMPLER, info.samplers);
    const VkDescriptorPoolCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = info.update_after_bind
                     ? VkDescriptorPoolCreateFlags{VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT}
                     : VkDescriptorPoolCreateFlags{},
        .maxSets = sets_per_pool,
        .poolSizeCount = static_cast<u32>(pool_cursor),
        .pPoolSizes = std::data(pool_sizes),
    };
#if defined(__PROSPERO__) // PS5
    // Kirby Air Riders (20 Nov 2026) ended at load: this pool's memory was refused once
    // (VK_ERROR_OUT_OF_DEVICE_MEMORY, largest free block of the pool shared with the CPU 13-23 MiB)
    // and the exception closed the app. The heap gives its empty arenas and spaces back when asked,
    // and the texture collector and the shader builds free memory meanwhile: ask, wait, try again
    // (up to ~8 s) before giving up as before.
    for (int attempt = 0;; ++attempt) {
        try {
            bank.pools.push_back(device.GetLogical().CreateDescriptorPool(create_info));
            if (attempt != 0) {
                std::fprintf(stderr, "EDEN_DESCRIPTOR_POOL_RETRY created after %d refusals\n", attempt);
            }
            return;
        } catch (const vk::Exception& exception) {
            const VkResult result = exception.GetResult();
            if ((result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_OUT_OF_HOST_MEMORY) ||
                attempt >= 40) {
                throw;
            }
            const std::size_t returned = eden_heap_trim ? eden_heap_trim() : 0;
            std::fprintf(stderr,
                         "EDEN_DESCRIPTOR_POOL_RETRY refused (%d), attempt %d: heap gave back %zu MiB, "
                         "waiting 200 ms\n",
                         int(result), attempt + 1, returned >> 20);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
#else
    bank.pools.push_back(device.GetLogical().CreateDescriptorPool(create_info));
#endif
}

DescriptorAllocator::DescriptorAllocator(const Device& device_, MasterSemaphore& master_semaphore_,
                                         DescriptorBank& bank_, VkDescriptorSetLayout layout_)
    : ResourcePool(master_semaphore_,
#if defined(__PROSPERO__) // PS5
                   BatchSets(bank_.info)
#else
                   SETS_GROW_RATE
#endif
                   ),
      device{&device_}, bank{&bank_}, layout{layout_},
#if defined(__PROSPERO__) // PS5
      batch{BatchSets(bank_.info)}
#else
      batch{SETS_GROW_RATE}
#endif
{}

VkDescriptorSet DescriptorAllocator::Commit() {
    const size_t index = CommitResource();
    return sets[index / batch][index % batch];
}

void DescriptorAllocator::Allocate(size_t begin, size_t end) {
    sets.push_back(AllocateDescriptors(end - begin));
}

vk::DescriptorSets DescriptorAllocator::AllocateDescriptors(size_t count) {
    const std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = bank->pools.empty() ? VkDescriptorPool{} : *bank->pools.back(),
        .descriptorSetCount = static_cast<u32>(count),
        .pSetLayouts = layouts.data(),
    };
#if defined(__PROSPERO__) // PS5
    if (bank->pools.empty()) {
        // The bank's first pool, at its first sets (PoolBudgetBytes).
        AllocatePool(*device, *bank);
    }
    allocate_info.descriptorPool = *bank->pools.back();
#endif
    vk::DescriptorSets new_sets = bank->pools.back().Allocate(allocate_info);
    if (!new_sets.IsOutOfPoolMemory()) {
        return new_sets;
    }
    // Our current pool is out of memory. Allocate a new one and retry
    AllocatePool(*device, *bank);
    allocate_info.descriptorPool = *bank->pools.back();
    new_sets = bank->pools.back().Allocate(allocate_info);
    if (!new_sets.IsOutOfPoolMemory()) {
        return new_sets;
    }
    // After allocating a new pool, we are out of memory again. We can't handle this from here.
    throw vk::Exception(VK_ERROR_OUT_OF_POOL_MEMORY);
}

DescriptorPool::DescriptorPool(const Device& device_, Scheduler& scheduler) {}
DescriptorPool::~DescriptorPool() = default;

DescriptorAllocator DescriptorPool::Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, std::span<const Shader::Info> infos, bool update_after_bind) {
    return Allocator(device, scheduler, layout, MakeBankInfo(infos, update_after_bind));
}

DescriptorAllocator DescriptorPool::Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const Shader::Info& info, bool update_after_bind) {
    return Allocator(device, scheduler, layout, MakeBankInfo(std::array{info}, update_after_bind));
}

DescriptorAllocator DescriptorPool::Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const DescriptorBankInfo& info) {
    return DescriptorAllocator(device, scheduler.GetMasterSemaphore(), Bank(device, info), layout);
}

DescriptorBank& DescriptorPool::Bank(const Device& device, const DescriptorBankInfo& reqs) {
    std::shared_lock read_lock{banks_mutex};
    const auto it = std::ranges::find_if(bank_infos, [&reqs](const DescriptorBankInfo& bank) {
        return std::abs(bank.score - reqs.score) < SCORE_THRESHOLD && bank.IsSuperset(reqs);
    });
    if (it != bank_infos.end()) {
        return *banks[std::distance(bank_infos.begin(), it)].get();
    }
    read_lock.unlock();

    std::unique_lock write_lock{banks_mutex};
    bank_infos.push_back(reqs);

    auto& bank = *banks.emplace_back(std::make_unique<DescriptorBank>());
    bank.info = reqs;
#if defined(__PROSPERO__) // PS5
    if (PoolBudgetBytes() == 0) {
        AllocatePool(device, bank);
    }
#else
    AllocatePool(device, bank);
#endif
    return bank;
}

} // namespace Vulkan
