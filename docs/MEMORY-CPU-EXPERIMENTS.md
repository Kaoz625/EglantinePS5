# Memory and CPU experiments, builds 01.219.021-027 (November 2026)

> **Resumen en español.** Entre el 7 y el 8 de noviembre de 2026 probamos en una PS5 (firmware
> 13.60) un conjunto de cambios de memoria y CPU, sobre un core más nuevo (Eglantine
> `nx2-experimental` `ac7882ac3f`). No se publicó ninguna de esas builds. **La swap de memoria
> del juego al SSD y mimalloc como heap de la app funcionan, pero no ayudan.** La swap tarda demasiado al
> devolver memoria (de 0,35 a 300 ms por cada bloque de 2 MB) y no evitó ningún cierre por falta
> de memoria. mimalloc retiene lo mismo que el heap de Sony (1,8-2,6 GB) y, al devolver memoria
> tan a menudo, destapó una carrera en nuestro código. Lo que sí funcionó: dibujar los task y mesh
> shaders con compute (el PR 569 de Lichii). Sin eso, cualquier dibujo mesh cuelga la GPU de la
> PS5. Con eso, Ocarina of Time pasó por primera vez de su vídeo de introducción. Ningún cambio subió los fps de forma
> medible. Los juegos grandes se quedan sin los 12 GB que la consola da a la app. De estas
> pruebas, la v1.219.028 solo lleva el arreglo de esa carrera.

## Summary

On 2026-11-07 and 2026-11-08 we ran seven experimental builds (01.219.021 to 01.219.027, branch
`experiment/memoria-cpu`) on one PS5 (firmware 13.60). They hold every change of a memory and CPU
plan at once, on a newer core (Eglantine `nx2-experimental` `ac7882ac3f`). None was released.
What we learned:

- **SSD swap of guest memory: works, does not help.** Slots came back from the SSD in 0.35 to
  300 ms per 2 MiB. No run avoided running out of memory. Packing cold memory in RAM with LZ4,
  as heydemoura/EglantinePS5 does, is about 0.8 ms per 2 MiB.
- **mimalloc as the app heap: works after one PS5 fix, does not help.** It held 1.8-2.6 GB,
  about what Sony's mspaces held (1.5-2.5 GB). It gives memory back so often that it exposed a
  race in our own memory code.
- **Task and mesh shaders drawn through compute (Lichii PR 569): needed on the PS5.** Any mesh
  draw through the PS5 Mesa's own path hung the GPU. Drawn through compute, Ocarina of Time got
  past its intro for the first time on that core.
- **No change raised the frame rate measurably.** The large games end when the 12 GiB pool
  (12224 MiB) that the console gives the app runs out.
- **Three bugs found and fixed:** one in mimalloc on the PS5, a race in our memory code, and a
  buffer lifetime bug we introduced ourselves. Only the race fix is in v1.219.028.

Every figure below is from the console logs of these runs (`heap.log`, `stderr.log`,
`eden_log.txt`, crash reports). Most are single runs, so read them as observations, not
benchmarks. A piece marked "not measured" was in the builds, but no run isolated its effect.

## What each piece did

| Piece | Result |
|---|---|
| SSD swap of cold guest memory (2 MiB slots to `user/swap/guest.swap`, restored on fault) | Works, does not help. Mario Kart World (MKW) on 021: 1060 MiB on the SSD, 1492 slots out, 962 back in, about 300 ms per slot read back. It ran out of memory at 810 s. After a gentler policy in 023, on 027: MKW 620 MiB on the SSD (500 out, 190 in, 7.6 ms per read, 56 ms per write); Ocarina of Time (OoT) 572 MiB (316 out, 30 in, 0.35-0.94 ms per read). Both still ran out of memory. Each read stalls the guest thread that faulted. |
| mimalloc v3.6.1 as the app heap, in 2 MiB commits | Works after one fix, does not help. On 021 every game crashed at start, because mimalloc takes thread ids from `%fs:0`, which is 0 on the PS5 (fixed: `pthread_self()`). On 024-027 it held 1.8-2.6 GB and gave back 1-6 GB over a run, but its peak stayed at the old heap's level. Its frequent give-backs exposed the race below. |
| Thread stacks of 512 KiB / 1 MiB instead of 2 MiB | Not measured. Expected 110-165 MiB in MKW. No stack overflow was seen in OoT, MKW or Bananza. |
| Presenting at the output resolution instead of 4K | Not measured. Expected about 120 MiB at 1080p. No visible problem. |
| Descriptor queue that grows as needed | Not measured. Expected up to 90 MiB. |
| Game in a clean process (LoadExec) | Works (`forwarded game: found`). Memory gain not measured. |
| BC1 instead of BC3 for MKW's alpha textures | Not measured separately. |
| Async shaders by default in MKW and Bananza | Not measured. |
| Skipping repeated GPU register writes | Not measured. |
| Thread placement with a topology probe fallback | The probe now finds 5 distinct cores (`EDEN_WORKER_TOPOLOGY ready=1 distinct_cores=5`). Frame rate effect not measured. |
| GPU timestamps per kind of work | Works. OoT's intro keeps the GPU 92% busy at 24 fps, and 98% of that time is 11 compute shaders. The largest takes 2 s of every 5 s. |
| Task and mesh draws through compute (zortos293/Lichii PR 569, ported as a core patch) | Works on the PS5: 29 to 64 emulated pipelines drawn per run, and no GPU hang. Before it, every task or mesh draw hung the GPU, with task emulation or mesh-only (`Waited N s for GPU tick T: the GPU has reached T-1`). Lichii merged the PR on 2026-11-07. |
| ASTC arrays re-compressed to BC1/BC3 in batches of layers | Works, with the fix below. OoT's 1365x1365, 90-layer array had needed an 894 MB decode image plus 224 MB of output at once, and ran out of memory. In batches each step holds at most 64 MB. |
| «Texture memory limit» at 3 GB (a setting, not code) | Makes things worse. In MKW the texture cache deleted 2372 images per 600 frames, and the game fell to 1-2 fps. heydemoura measured the same (8-11 s stalls with a 3-3.5 GiB cache). The cache compares its limit with all graphics memory, not only textures: 860 MB of images against 5.8 GB counted. |

## Bugs found on the way

- **mimalloc and `%fs:0`.** On the PS5, `%fs:0` is not a thread pointer: it reads 0. mimalloc's
  thread id (`MI_PRIM_THREAD_ID`) must come from `pthread_self()`, or its run-once guard skips
  process init. Every game crashed at start (SIGSEGV at address 0 in
  `_mi_thread_init_with_heap`).
- **Unmap, then reserve: a race.** `DecommitMemoryRange` and the fastmem window's `UnmapChunks`
  gave memory back with `munmap`, then `sceKernelReserveVirtualRange(MAP_FIXED)` on the same
  range.
  - In between, the range was free. A mapping without a fixed address (the zeroing view of the
    guest memory reserve, at the same hint) could land there, and the reservation then replaced
    it.
  - Crash: SIGSEGV code 2 in `memset`, in `FillReserve`, at the start of that view.
  - This also proves that a fixed reservation replaces a live direct-memory mapping in one call.
    So the fix (`ReserveOver`, `src/memory_pages.cpp`) reserves over the mapping directly, and
    keeps the old order only as a fallback.
  - The same code is in heydemoura/EglantinePS5 `main`. It only shows when memory is given back
    often, as with mimalloc. v1.219.028 has the fix.
- **A staging buffer used past its tick (our own bug).** The first batched ASTC version took
  its output buffer before the first batch, and each batch's decode submits (`AssembleInto`
  with `wait`).
  - The staging pool then counted the buffer free after the first submission, and deleted it
    while later batches still used it.
  - Crash: SIGSEGV in RADV's `vkCmdCopyBufferToImage` (`radv_get_copy_flags_from_bo` on a freed
    buffer).
  - Fix: request it deferred and free it after the last batch, and let only the last batch's
    decode submit. Any buffer used across a scheduler flush needs the same care.

## Where the memory goes when a game runs out

| Game, build | Guest memory | Graphics (driver allocator) | App heap | Swap on SSD | End |
|---|---|---|---|---|---|
| OoT, 027 | 4.7 GB | 3.9 GB | 2.3-2.6 GB (mimalloc; 9553 commits refused) | 572 MB | crash at 584 s, in the shader compiler (NIR) when an allocation failed |
| MKW, 027 | 5.4-5.5 GB | 3.3 GB | 2.1 GB (mimalloc) | 620 MB | 1-2 fps from about 130 s, closed by hand at 241 s |

On top of these come the JIT code caches (about 0.5 GB) and the guest memory reserve (0.5 GB).
Together they fill the 12224 MiB pool. heydemoura reached the same conclusion for MKW
(`docs/MKW-MEMORY-BUDGET.md` in heydemoura/EglantinePS5): the guest's ~4.4 GB, the driver's
~4 GB and the emulator's ~1.5 GB fill the pool, and MKW still runs out after 4-7 minutes with
all of their memory work.

Two figures in the logs mislead:
- `Texture cache: device memory N MiB` and `VMA heap 1: usage` count the driver's allocations
  twice. Use the allocator's figure in `EDEN_GRAPHICS_MEMORY` instead.
- In the PS5 Mesa, `agc_ms` (time blocked in the GPU submit) only shows that the submit waited.
  GPU timestamps show whether the GPU was busy.

## Frame rates seen

| Game | On these builds | Before (v1.219.007-020) | PC reference (MythosOfs/Orchid: 5800X3D + RX 9070 XT, 1x) |
|---|---|---|---|
| Donkey Kong Bananza | 8-13 fps over 3 min (027) | 10-25 fps, about 15 in open areas | 30, 15-25 in heavy areas |
| Mario Kart World | 23-40 fps, then out of memory at about 2 min | 25-43 fps in a race, out of memory after 1-3 min | 30-40 |
| Zelda: Ocarina of Time | 24 fps in the intro (GPU 92% busy), about 15 fps in the first scene, then out of memory | about 18 fps on v1.219.015 (one tester, older core) | not listed |

Stable 30 fps without crashes is not in reach for these three games on a base PS5 with this
approach. Even that PC does not keep 30 fps in Bananza's heavy areas, and the 12 GiB pool is
smaller than what these games plus an emulator need.

## Advice for other PS5 ports

- Do not swap guest memory to the SSD. Compress it in RAM (LZ4), as heydemoura does, or not at
  all.
- Replacing the app heap with mimalloc buys nothing measurable. If you try it anyway, take
  thread ids from `pthread_self()`, not `%fs:0`.
- Never unmap a range and then map it with `MAP_FIXED`. Reserve or map over it in one call.
- Keep the texture cache's limits above what the other graphics memory already takes. Below
  that, it deletes and re-uploads textures every frame.
- On the PS5 Mesa used here (mihawk-99/PS5_Mesa `f5610fe27`), do not draw task or mesh shaders
  with the driver's own path. Draw them through compute.
- Change one thing per build and run the same scene twice. These builds combined about ten
  changes on a new core, so we can say which ones crashed, but not which ones helped.

The code of every piece is on branch `experiment/memoria-cpu` of viejunerd/Eglantine-PS5-
(frontend commits `2e92053` to `ff12f86`, one per build, and core patches `0082`-`0090`). Most pieces can be turned
off with an environment variable:

- `EDEN_HEAP_MIMALLOC=0`, `EDEN_GUEST_SWAP=0`
- `EDEN_FIBER_STACK_KIB=2048`, `EDEN_PS5_SCANOUT_4K=1`
- `EDEN_PS5_CLEAN_LAUNCH=0`, `EDEN_PS5_PLACEMENT=0`, `EDEN_PS5_ASTC_BC1=0`
- `EDEN_NX2_SKIP_SAME_STATE=0`, `EDEN_NX2_FORCE_MESH_FALLBACK=0`
