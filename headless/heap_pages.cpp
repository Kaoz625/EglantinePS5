// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/sparse_large_vector.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern "C" int sceKernelDebugOutText(int channel, const char* text);

namespace Common {
void* ReserveMemoryRange(std::size_t size) noexcept;
void* ReserveAddressRange(std::size_t size) noexcept;
bool CommitMemoryRange(void* address, std::size_t size) noexcept;
bool CommitMemoryRangeFrom(void* address, std::size_t size, bool high) noexcept;
int DecommitMemoryRange(void* address, std::size_t size) noexcept;
int PageOutRange(void* address, std::size_t size) noexcept;
int PageInRange(void* address, std::size_t size) noexcept;
void ReleaseMemoryRange(void* address, std::size_t size) noexcept;
} // namespace Common

// Out of memory for the heap: the guest memory service packs cold guest memory to give some back
// (src/guest_memory_service.inc; never waits, the heap may hold its growth lock), and the heap
// asks once more. Before, a refused growth made an allocation fail (a worker thread then read
// address 0x199).
extern "C" [[gnu::weak]] std::size_t eden_memory_recover_try(std::size_t bytes) noexcept;
template <typename Try>
static auto WithRecovery(std::size_t size, Try&& attempt) {
    auto result = attempt();
    if (!result && &eden_memory_recover_try != nullptr &&
        eden_memory_recover_try(size + (std::size_t{8} << 20)) != 0)
        result = attempt();
    return result;
}

// H41: one recovery was not enough late in a Mario Kart World session (PHAT5 H39 L2, race 3):
// the heap's 32 MiB growth was refused right after a recovery freed 40 MiB (other threads took it
// first) and the app ended with std::bad_alloc. A growth or a large block in pieces now asks again
// for up to EDEN_HEAP_WAIT_MS of wall time (default 2000; 0 = once, as H40), every 20 ms.
// Wall time, not tries: each try may itself take ~100 ms (playbook #16).
static long HeapWaitMs() {
    static long value = -1;
    if (value < 0) {
        const char* text = std::getenv("EDEN_HEAP_WAIT_MS");
        long ms = text != nullptr ? std::strtol(text, nullptr, 10) : 2000;
        value = ms < 0 ? 0 : (ms > 10000 ? 10000 : ms);
    }
    return value;
}
static std::atomic<unsigned> heap_wait_lines{0}, heap_waits{0}, heap_waits_ok{0};
extern "C" void eden_heap_wait_stats(unsigned* waits, unsigned* ok) {
    *waits = heap_waits.load(std::memory_order_relaxed);
    *ok = heap_waits_ok.load(std::memory_order_relaxed);
}
template <typename Try>
static auto WithRecoveryWait(std::size_t size, Try&& attempt) {
    auto result = WithRecovery(size, attempt);
    const long wait_ms = HeapWaitMs();
    if (result || wait_ms == 0 || &eden_memory_recover_try == nullptr) return result;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(wait_ms);
    unsigned tries = 0;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        if (eden_memory_recover_try(size + (std::size_t{8} << 20)) == 0) usleep(20000);
        result = attempt();
        ++tries;
    }
    heap_waits.fetch_add(1, std::memory_order_relaxed);
    if (result) heap_waits_ok.fetch_add(1, std::memory_order_relaxed);
    if (heap_wait_lines.fetch_add(1, std::memory_order_relaxed) < 64) {
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start).count();
        char line[128];
        const int length = std::snprintf(line, sizeof(line), "EDEN_HEAP_WAIT bytes=%zu tries=%u ms=%lld ok=%d\n",
                                         size, tries, ms, result ? 1 : 0);
        if (length > 0) sceKernelDebugOutText(0, line);
    }
    return result;
}

#ifdef PS5_NATIVE
extern "C" std::int64_t sceKernelGetDirectMemorySize();
extern "C" std::int32_t sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t*,
                                                           std::size_t*);
#endif
// H49: a block of its own needs ONE free piece of the pool that large. Mario Kart World (PHAT5 H47b S1, race-2
// load) asked 11 times for 42-50 MiB blocks with the pool's largest free piece 2-4 MiB: each time the recovery
// packed ~50 MiB of guest memory (in 2 MiB slots scattered over the pool, so it never made one piece that
// large), then the request went to eden_heap_split_pages anyway, which recovers again. With no free piece as
// large as the request, the recovery is skipped here (the split path's recovery stays).
// EDEN_HEAP_PAGES_RECOVER=1 restores the H48 behaviour (always recover).
static std::atomic<unsigned> heap_pages_skips{0};
static bool PoolHasPieceFor(std::size_t size) noexcept {
#ifdef PS5_NATIVE
    static const bool always = [] {
        const char* text = std::getenv("EDEN_HEAP_PAGES_RECOVER");
        return text != nullptr && text[0] == '1';
    }();
    if (always) return true;
    const std::int64_t total = sceKernelGetDirectMemorySize();
    std::int64_t start = 0;
    std::size_t largest = 0;
    if (total <= 0 || sceKernelAvailableDirectMemorySize(0, total, std::size_t{2} << 20, &start, &largest) != 0)
        return true; // not known: as before
    // AllocatePagesAt adds one 2 MiB lead page to a large block.
    return largest >= size + (std::size_t{2} << 20);
#else
    (void)size;
    return true;
#endif
}

// Reuse the qualified, owned direct-memory backend for the C mspace heap.
extern "C" void* eden_heap_pages(std::size_t size) {
    void* result = Common::AllocateMemoryPages(size);
    if (result != nullptr || &eden_memory_recover_try == nullptr) return result;
    if (!PoolHasPieceFor(size)) {
        const unsigned skips = heap_pages_skips.fetch_add(1, std::memory_order_relaxed);
        if (skips < 32 || skips % 64 == 0) {
            char line[112];
            const int length = std::snprintf(line, sizeof(line),
                                             "EDEN_HEAP_PAGES_NO_PIECE bytes=%zu skips=%u (no recovery, split pages next)\n",
                                             size, skips + 1);
            if (length > 0) sceKernelDebugOutText(0, line);
        }
        return nullptr;
    }
    if (eden_memory_recover_try(size + (std::size_t{8} << 20)) != 0) result = Common::AllocateMemoryPages(size);
    return result;
}
extern "C" void eden_heap_pages_free(void* base, std::size_t size) {
    Common::FreeMemoryPages(base, size);
}
// The heap that grows (headless/heap_arenas.inc): address space first, memory piece by piece.
extern "C" void* eden_heap_reserve(std::size_t size) {
    return Common::ReserveMemoryRange(size);
}
extern "C" int eden_heap_commit(void* address, std::size_t size) {
    return WithRecoveryWait(size, [&] { return Common::CommitMemoryRange(address, size); }) ? 0 : -1;
}
// A piece that holds nothing goes back to the system; its address space stays the heap's.
// 1: given back, 0: nothing done, -1: given back in part (not to be used again).
extern "C" int eden_heap_decommit(void* address, std::size_t size) {
    return Common::DecommitMemoryRange(address, size);
}
// A released arena's memory (headless/heap_arenas.inc): part of a committed block, in 16 KiB pages.
extern "C" int eden_heap_page_out(void* address, std::size_t size) {
    return Common::PageOutRange(address, size);
}
extern "C" int eden_heap_page_in(void* address, std::size_t size) {
    return WithRecovery(size, [&] { return Common::PageInRange(address, size) == 1; }) ? 1 : 0;
}
// A large block whose memory the pool has in no single free block: address space of its own,
// with memory behind it in blocks of whatever size the pool has (zeroed), given back whole.
extern "C" void* eden_heap_split_pages(std::size_t size) {
    constexpr std::size_t large_page = std::size_t{2} << 20;
    if (size == 0 || size > ~std::size_t{0} - large_page) return nullptr;
    const std::size_t span = (size + large_page - 1) / large_page * large_page;
    void* address = Common::ReserveAddressRange(span);
    if (address == nullptr) return nullptr;
    if (!WithRecoveryWait(span, [&] { return Common::CommitMemoryRangeFrom(address, span, false); })) {
        Common::ReleaseMemoryRange(address, span);
        return nullptr;
    }
    return address;
}
extern "C" void eden_heap_split_pages_free(void* address, std::size_t size) {
    constexpr std::size_t large_page = std::size_t{2} << 20;
    Common::ReleaseMemoryRange(address, (size + large_page - 1) / large_page * large_page);
}
