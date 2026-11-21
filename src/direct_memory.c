// SPDX-License-Identifier: GPL-3.0-or-later
// Where the app's long-lived memory goes in the pool the CPU shares with the GPU.
//
// The system gives out direct memory from the bottom of the pool: the lowest room that fits.
// A game's own memory (4 GiB) and its graphics come and go with each session, while two kinds
// of blocks stay for good: the pieces the heap grows by (headless/heap_arenas.inc) and the
// stacks of threads (the Payload SDK takes 2 MiB of direct memory for each, and keeps freed
// ones for the next threads). Taken from the bottom like the rest, those landed between a
// running game's graphics memory, and when the game ended they were left spread over the
// middle of the pool. The next game's graphics get the largest free stretch, which was then
// much smaller than what was free in total, and smaller again after every game.
//
// So blocks that stay are taken from the top of the pool downwards, and everything a session
// takes and gives back is left to the system as before. The link wraps the calls
// (tools/link-headless-native.sh): the heap asks for the top itself (eden_direct_high), and
// the other blocks that stay are known by their size: a thread's stack is 2 MiB, and so is the
// one block the graphics driver keeps for good (its zero block, taken in the middle of a game
// and then left where it was). The driver's command ring and marker page are known by who
// asks. The app's own 2 MiB blocks, the slots of the guest page tables, go back when a game
// ends: they ask below the wrap (eden_direct_low) and stay where the system puts them.
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

int __real_sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int __real_sceKernelReleaseDirectMemory(int64_t, size_t);
int64_t sceKernelGetDirectMemorySize(void);

#define EDEN_DIRECT_STEP ((int64_t)2 << 20)       /* blocks at the top are placed in 2 MiB steps */
#define EDEN_DIRECT_SMALL ((size_t)2 << 20)       /* a thread's stack; the driver's zero block */

static atomic_flag eden_direct_lock = ATOMIC_FLAG_INIT;
static int64_t eden_direct_floor;                 /* the lowest block taken at the top; 0: none yet */
static int64_t eden_direct_holes[1024];           /* 2 MiB blocks given back above the floor */
static unsigned eden_direct_hole_count;

static void eden_direct_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&eden_direct_lock, memory_order_acquire)) {}
}
static void eden_direct_release(void) { atomic_flag_clear_explicit(&eden_direct_lock, memory_order_release); }

/* Who holds the pool (Kirby Air Riders, 21 Nov 2026: ~3 GB of the 12 GiB pool at the out-of-memory
 * was in none of the reports - driver, guest, heap, JIT). Every block taken through this file is
 * charged to its caller (the return address of the call into it) until it goes back, and
 * eden_direct_census prints the callers next to what the system says is taken in all: the
 * difference is memory taken by code that does not come through here. A measurement only. */
#include <stdio.h>
struct DirectMemoryRegion { int64_t start, end; int32_t type; };
int sceKernelDirectMemoryQuery(int64_t, int, struct DirectMemoryRegion *, size_t);
#define EDEN_CENSUS_OWNERS 96u
#define EDEN_CENSUS_BLOCKS 65536u /* power of two */
static struct { uintptr_t caller; int64_t bytes, peak; unsigned blocks; unsigned char via; } eden_census_owner[EDEN_CENSUS_OWNERS];
static unsigned eden_census_owner_count;
static struct { int64_t key; int64_t size; unsigned short owner; } eden_census_block[EDEN_CENSUS_BLOCKS]; /* key: start + 1, 0 empty */
static int64_t eden_census_lost_bytes, eden_census_unmatched_bytes;
static unsigned eden_census_lost, eden_census_unmatched;
static atomic_flag eden_census_lock = ATOMIC_FLAG_INIT;
static void eden_census_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&eden_census_lock, memory_order_acquire)) {}
}
static void eden_census_release(void) { atomic_flag_clear_explicit(&eden_census_lock, memory_order_release); }
static unsigned eden_census_slot(int64_t key) {
  return (unsigned)(((uint64_t)key >> 14) * 0x9E3779B97F4A7C15ull >> 48) & (EDEN_CENSUS_BLOCKS - 1);
}
static void eden_census_insert(int64_t start, int64_t size, unsigned owner) {
  unsigned at = eden_census_slot(start + 1);
  for (unsigned probes = 0; probes < EDEN_CENSUS_BLOCKS; ++probes, at = (at + 1) & (EDEN_CENSUS_BLOCKS - 1))
    if (eden_census_block[at].key == 0) {
      eden_census_block[at].key = start + 1;
      eden_census_block[at].size = size;
      eden_census_block[at].owner = (unsigned short)owner;
      return;
    }
  ++eden_census_lost;
  eden_census_lost_bytes += size;
}
/* Linear probing, deletion by moving later entries back (no tombstones). */
static int eden_census_remove(int64_t start, int64_t *size, unsigned *owner) {
  unsigned at = eden_census_slot(start + 1);
  for (unsigned probes = 0; probes < EDEN_CENSUS_BLOCKS; ++probes, at = (at + 1) & (EDEN_CENSUS_BLOCKS - 1)) {
    if (eden_census_block[at].key == 0) return 0;
    if (eden_census_block[at].key != start + 1) continue;
    *size = eden_census_block[at].size;
    *owner = eden_census_block[at].owner;
    unsigned hole = at;
    for (unsigned next = (at + 1) & (EDEN_CENSUS_BLOCKS - 1); eden_census_block[next].key != 0;
         next = (next + 1) & (EDEN_CENSUS_BLOCKS - 1)) {
      const unsigned home = eden_census_slot(eden_census_block[next].key);
      /* Move it back if its home is not in (hole, next]. */
      if (((next - home) & (EDEN_CENSUS_BLOCKS - 1)) >= ((next - hole) & (EDEN_CENSUS_BLOCKS - 1))) {
        eden_census_block[hole] = eden_census_block[next];
        hole = next;
      }
    }
    eden_census_block[hole].key = 0;
    return 1;
  }
  return 0;
}
/* via: 0 the wrapped system call, 1 eden_direct_high, 2 eden_direct_low. */
static void eden_census_take(uintptr_t caller, unsigned char via, int64_t start, size_t size) {
  eden_census_acquire();
  unsigned owner = 0;
  while (owner < eden_census_owner_count &&
         (eden_census_owner[owner].caller != caller || eden_census_owner[owner].via != via))
    ++owner;
  if (owner == eden_census_owner_count) {
    if (owner < EDEN_CENSUS_OWNERS) {
      eden_census_owner[owner].caller = caller;
      eden_census_owner[owner].via = via;
      ++eden_census_owner_count;
    } else
      owner = EDEN_CENSUS_OWNERS - 1; /* the last entry takes everyone after it */
  }
  eden_census_owner[owner].bytes += (int64_t)size;
  if (eden_census_owner[owner].bytes > eden_census_owner[owner].peak)
    eden_census_owner[owner].peak = eden_census_owner[owner].bytes;
  ++eden_census_owner[owner].blocks;
  eden_census_insert(start, (int64_t)size, owner);
  eden_census_release();
}
static void eden_census_give(int64_t start, size_t size) {
  eden_census_acquire();
  int64_t held = 0;
  unsigned owner = 0;
  if (eden_census_remove(start, &held, &owner)) {
    const int64_t given = (int64_t)size < held ? (int64_t)size : held;
    eden_census_owner[owner].bytes -= given;
    if (given == held) --eden_census_owner[owner].blocks;
    else eden_census_insert(start + given, held - given, owner); /* the front went back */
  } else {
    ++eden_census_unmatched; /* a part from the middle or the end of a block, or one taken elsewhere */
    eden_census_unmatched_bytes += (int64_t)size;
  }
  eden_census_release();
}
/* EDEN_DMEM_CENSUS: taken (the system's view, all of the pool), tracked (through here) and the
 * difference; then each caller holding 8 MiB or more. Addresses are printed against
 * eden_direct_census's own address, so one build's lines compare with each other. */
static uintptr_t eden_direct_census_base;
void eden_direct_census(FILE *out, const char *why) {
  if (!eden_direct_census_base) eden_direct_census_base = (uintptr_t)&eden_direct_census;
  const int64_t total = sceKernelGetDirectMemorySize();
  int64_t taken = 0, offset = 0;
  unsigned regions = 0;
  struct DirectMemoryRegion region;
  while (offset < total && regions < 65536 &&
         sceKernelDirectMemoryQuery(offset, 1, &region, sizeof(region)) == 0 && region.end > offset) {
    taken += region.end - (region.start > offset ? region.start : offset);
    offset = region.end;
    ++regions;
  }
  /* Copied under the lock and printed after it: printing may grow the heap, which comes here. */
  struct { uintptr_t caller; int64_t bytes, peak; unsigned blocks; unsigned char via; } owners[EDEN_CENSUS_OWNERS];
  eden_census_acquire();
  const unsigned count = eden_census_owner_count;
  for (unsigned i = 0; i < count; ++i) {
    owners[i].caller = eden_census_owner[i].caller;
    owners[i].bytes = eden_census_owner[i].bytes;
    owners[i].peak = eden_census_owner[i].peak;
    owners[i].blocks = eden_census_owner[i].blocks;
    owners[i].via = eden_census_owner[i].via;
  }
  const unsigned lost = eden_census_lost, unmatched = eden_census_unmatched;
  const int64_t lost_bytes = eden_census_lost_bytes, unmatched_bytes = eden_census_unmatched_bytes;
  eden_census_release();
  int64_t tracked = 0;
  for (unsigned i = 0; i < count; ++i) tracked += owners[i].bytes;
  fprintf(out, "EDEN_DMEM_CENSUS why=%s total_mib=%lld taken_mib=%lld tracked_mib=%lld untracked_mib=%lld regions=%u owners=%u lost=%u/%lldMiB unmatched=%u/%lldMiB base=%p\n",
          why, (long long)(total >> 20), (long long)(taken >> 20), (long long)(tracked >> 20),
          (long long)((taken - tracked) >> 20), regions, count, lost, (long long)(lost_bytes >> 20), unmatched,
          (long long)(unmatched_bytes >> 20), (void *)eden_direct_census_base);
  static const char *const via_names[] = {"sys", "high", "low"};
  extern void eden_radv_private_radv_ps5_memory_alloc(void) __attribute__((weak));
  const uintptr_t radv = (uintptr_t)&eden_radv_private_radv_ps5_memory_alloc;
  for (unsigned i = 0; i < count; ++i) {
    if (owners[i].bytes < ((int64_t)8 << 20) && owners[i].peak < ((int64_t)64 << 20)) continue;
    const uintptr_t caller = owners[i].caller;
    fprintf(out, "EDEN_DMEM_OWNER via=%s caller=%+lld%s mib=%lld peak_mib=%lld blocks=%u\n", via_names[owners[i].via],
            (long long)(caller - eden_direct_census_base),
            radv && caller >= radv && caller < radv + 2048 ? " (radv_ps5_memory_alloc)" : "",
            (long long)(owners[i].bytes >> 20), (long long)(owners[i].peak >> 20), owners[i].blocks);
  }
}

/* Memory from as high in the pool as there is room; from anywhere when the top has none. */
static int eden_direct_high_take(size_t size, size_t alignment, int type, int64_t *start) {
  const int64_t total = sceKernelGetDirectMemorySize();
  const int64_t bytes = (int64_t)size;
  if (bytes <= 0 || total <= 0) return __real_sceKernelAllocateDirectMemory(0, total, size, alignment, type, start);
  eden_direct_acquire();
  /* A block given back earlier left a hole of its size. */
  while (size == EDEN_DIRECT_SMALL && eden_direct_hole_count != 0) {
    const int64_t hole = eden_direct_holes[--eden_direct_hole_count];
    if (__real_sceKernelAllocateDirectMemory(hole, hole + bytes, size, alignment, type, start) == 0) {
      eden_direct_release();
      return 0;
    }
  }
  /* Below what is at the top already; past what something else has put there, for a while. */
  int64_t next = eden_direct_floor != 0 ? eden_direct_floor : total;
  for (unsigned tries = 0; tries < 512 && next >= bytes; ++tries) {
    const int64_t at = (next - bytes) / EDEN_DIRECT_STEP * EDEN_DIRECT_STEP;
    next = at;
    if (__real_sceKernelAllocateDirectMemory(at, at + bytes, size, alignment, type, start) == 0) {
      eden_direct_floor = at;
      eden_direct_release();
      return 0;
    }
  }
  eden_direct_release();
  return __real_sceKernelAllocateDirectMemory(0, total, size, alignment, type, start);
}

/* As the system places it: for what a session takes and gives back. */
int eden_direct_high(size_t size, size_t alignment, int type, int64_t *start) {
  const int result = eden_direct_high_take(size, alignment, type, start);
  if (result == 0 && start) eden_census_take((uintptr_t)__builtin_return_address(0), 1, *start, size);
  return result;
}
int eden_direct_low(size_t size, size_t alignment, int type, int64_t *start) {
  const int result = __real_sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, alignment, type, start);
  if (result == 0 && start) eden_census_take((uintptr_t)__builtin_return_address(0), 2, *start, size);
  return result;
}

/* The Vulkan driver makes its command ring (16 MiB) and, right after it, the ring's marker page
 * once, with the first device, and keeps them until the app ends. Taken from the bottom like a
 * session's memory they landed above the first game's (the ring at 4.6 GiB), and every later
 * game found the pool's largest free stretch cut from 10.3 GiB to 5.7. They are known by who
 * asks (the driver's allocator; no such symbol in a build without that driver), by their sizes
 * and by being the first of those sizes in that order. */
extern void eden_radv_private_radv_ps5_memory_alloc(void) __attribute__((weak));
#define EDEN_DIRECT_RING ((size_t)16 << 20)
#define EDEN_DIRECT_PAGE ((size_t)16 << 10)
static atomic_int eden_direct_driver_kept; /* 0: neither yet; 1: the ring; 2: the marker page too */

static int eden_direct_driver_keeps(size_t size, const void *caller) {
  const char *const allocator = (const char *)(uintptr_t)&eden_radv_private_radv_ps5_memory_alloc;
  if (!allocator || (const char *)caller < allocator || (const char *)caller >= allocator + 2048) return 0;
  int kept = atomic_load_explicit(&eden_direct_driver_kept, memory_order_relaxed);
  if (kept == 0 && size == EDEN_DIRECT_RING)
    return atomic_compare_exchange_strong(&eden_direct_driver_kept, &kept, 1);
  if (kept == 1 && size == EDEN_DIRECT_PAGE)
    return atomic_compare_exchange_strong(&eden_direct_driver_kept, &kept, 2);
  return 0;
}

int __wrap_sceKernelAllocateDirectMemory(int64_t from, int64_t to, size_t size, size_t alignment, int type,
                                         int64_t *start) {
  /* A thread's stack or the driver's zero block, asked for anywhere in the pool; and the
   * driver's command ring and marker page. */
  const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  const int result = from == 0 && start && to >= sceKernelGetDirectMemorySize() &&
                             (size == EDEN_DIRECT_SMALL || eden_direct_driver_keeps(size, (const void *)caller))
                         ? eden_direct_high_take(size, alignment, type, start)
                         : __real_sceKernelAllocateDirectMemory(from, to, size, alignment, type, start);
  if (result == 0 && start) eden_census_take(caller, 0, *start, size);
  return result;
}

int __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t size) {
  const int result = __real_sceKernelReleaseDirectMemory(start, size);
  if (result != 0) return result;
  eden_census_give(start, size);
  eden_direct_acquire();
  if (size == EDEN_DIRECT_SMALL && eden_direct_floor != 0 && start >= eden_direct_floor &&
      eden_direct_hole_count < sizeof(eden_direct_holes) / sizeof(eden_direct_holes[0]))
    eden_direct_holes[eden_direct_hole_count++] = start;
  eden_direct_release();
  return result;
}
