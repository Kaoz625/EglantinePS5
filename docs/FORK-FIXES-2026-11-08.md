# Fixes from other Switch 2 projects (survey 8 Nov 2026)

Survey of zortos293/Lichii, seedds/nxnative (+ forks), niakw/ProsperoEden-Encore and Swordpdf/ProsperoEden
for fixes our build (viejunerd v1.219.028 + core 0ca2032 + patches) lacks. Status column: ported = in a patch here.

## Ported
| Patch | Source | Fix |
|---|---|---|
| 0085 | Lichii aa0acb3e #311, 2e69168a #432, 12fc47e8 #609, f99fb8ab #185, 57a6334e #444 | clear-skip coverage, NaN-defined min/max, dma_increment_once race, deferred clears before writes, multi-range buffer leak |
| 0086 | nxnative services.md "camera commands of SDK 21's own camera module" | nx2:camera = empty camera port (Star Fox null object) |
| 0087 | Swordpdf/ProsperoEden e34338dd | NaN pixel colour stored as 0 |

## Next (Lichii)
821ccd57 #700 (invalidate every block a cached write touches; query_cache_base.h conflicts), 6bb54dd8 #425 (usage tracker; conflicts),
79c15169 #353 (don't write RT components the shader doesn't output - AMD garbage), bf4c42e2 #679 (easy AMD fixes),
451f3099 #818 (stale guest memory into new images), 28d99d3d #273 (AMD depth sampling), 7223507e #329 (32-wide warps),
18cfe855 #295 (misaligned 64/128-bit storage), 5358d3db #440 (fail an image that doesn't fit), 23e91fa4 #575 (host mip limits),
aa26fafa #817, 3099486e #752, 47a1f19d #481, f992933e #465, 32886f75 #436 (Orbitals hang), ce9be9d1/5acbea8f (MKW), 455f635a #600 (audio).
Do NOT take: #606/#5/#67/#582/#583 (NVIDIA/coop-matrix), #645/#698/#232/#621-Windows, #756, #769/#530/#418 (affinity), #268 (texture threshold).

## Next (nxnative, port by meaning)
- seedds fdb3710d: Switch 2 64x8 tile swizzle swaps offset bits 5/6 -> 4x4 block weave (Hyrule/Star Fox blocks?).
- Aznracer 6542d07e: oe::GetCurrentGraphicsScale tier 0 in both modes (Star Fox rendered 768x432); 0b91ce92 vi::ListDisplays largest layer (960x540); e9d2e537 sRGB image load/store raw (Star Fox washed out).
- seedds 67f1d005 SDK 21 sockets/Poll + chat::GetChattingUid; 7bccfc88 BMSK 0x21b + 0x925 (Star Fox shaders).
- Splatoon: renderArea = scissor on >=16M-pixel targets; 6d5ed718 occlusion query list grows (1-2 fps at first boss); 1b9fbea4 64/128-bit storage reads as one vector; report tables grow (black scene with render_cond).
- 3fe755a2/16776913 LDS/STS stride bits 78-79 (LDSM = transpose) + subgroup barrier (8x8 garbage lattice: Sonic/Star Fox?).
- b5948342 MUFU.SIN/COS fraction first; Aznracer 34d04efb float atomic add (OoT washed out); e07cba1c MKW counter reset; 75e5ddd8 CPU-written textures from storage at first use (OoT holes); jgrose a650827a OoT 31.25 MHz timestamps; 7d8dcdf8 OoT/MKW frame ratio offsets.
- Settings: OoT mem_map_limit; Splatoon alias/reserve sizes; Tennis alias region.

## Next (Encore / ProsperoEden)
cc128c9a JIT code pages given back after a cache clear (memory); 66159990 + 050c5e53 keep GPU-written textures when staging fails;
63f4cc34/74947a13 DrawTexture without shader translation under the texture lock; 1db1c57d/4caa15c2 scheduler rollover drops;
f0114ba6 queue frees popped GPU commands; 18627649 RADV transfer split (Mesa rebuild). Encore itself draws no Switch 2 game yet.
