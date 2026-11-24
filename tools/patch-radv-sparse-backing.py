#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Device memory a sparse resource is bound over is never given back by the trim (H45).

patch-radv-trim.py counts, per 2 MiB granule of a buffer object, the images and buffers bound
over it through vkBind*Memory, and gives back granules with none. A sparse bind
(vkQueueBindSparse -> radv_ps5_buffer_virtual_bind) maps the backing memory's direct memory at
the sparse range's own address and was never counted. Eden's sparse tiles and layers
(Image::BindTiles / BindLayers) take that memory from VMA: tiles from a separate pool, but layers
over 8 MiB, and tiles whenever the pool cannot grow (the pool is short exactly then), from the
default blocks that images share. A granule holding only sparse tiles looked unused (and a
granule never bound by vkBind* skipped even the 2 s quarantine), so the trim released its direct
memory while the sparse range still mapped it: the GPU then reads an unmapped page in RADV's
device region (Mario Kart World H44 R1, 23 Nov: GPU_FAULT_PAGE_FAULT_ASYNC, client TCP read,
0x41b1aac000, during heavy trims; the link is reasoned from the code, not yet matched by the
buffer object log). Memory that backs a sparse bind is now marked and the trim skips it; the
mark is set under the trim lock before the range maps it, so a trim already under way either
finished first (the bind then fails cleanly: the memory is in pieces) or never sees it.
stderr: "EDEN_DEFER_FREE radv: N buffer objects back sparse resources" (first 8, then every 64th)
and, per trim that skipped some, "EDEN_DEFER_FREE radv trim: kept K sparse-backing objects".
Applied after patch-radv-trim.py. Patches the build export, never the Mesa checkout.
"""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
source = (Path(sys.argv[1]) if len(sys.argv) > 1 else
          root.parent / 'mihawk-vulkan-review/.deps/work/radv-src')
MARK = 'EglantinePS5 sparse backing kept'

MARK_FN = r'''/* ''' + MARK + r''' (tools/patch-radv-sparse-backing.py): memory a sparse range maps
 * (vkQueueBindSparse) is never trimmed; the trim's counts see only vkBind*Memory. */
static uint32_t radv_ps5_sparse_backing_count;

void
radv_ps5_memory_sparse_backing(struct radv_ps5_memory *memory)
{
   radv_ps5_trim_acquire();
   if (!memory->sparse_backing) {
      memory->sparse_backing = true;
      const uint32_t n = ++radv_ps5_sparse_backing_count;
      if (n <= 8 || n % 64 == 0)
         fprintf(stderr,
                 "EDEN_DEFER_FREE radv: %u buffer objects back sparse resources, never trimmed "
                 "(this one %" PRIu64 " MiB at %p%s)\n",
                 n, memory->bytes >> 20, (void *)memory->cpu, memory->uses ? ", tracked" : "");
   }
   radv_ps5_trim_release();
}

'''

edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.h': [(
        '''   uint32_t *idle_ms;
};

bool radv_ps5_memory_use(''',
        '''   uint32_t *idle_ms;
   /* A sparse range maps it (''' + MARK + '''): the trim leaves it alone. */
   bool sparse_backing;
};

void radv_ps5_memory_sparse_backing(struct radv_ps5_memory *memory);
bool radv_ps5_memory_use(''')],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''bool
radv_ps5_memory_use(struct radv_ps5_memory *memory, uint64_t offset, uint64_t size, int delta)
{
   if (size == 0 || offset >= memory->bytes)''',
         MARK_FN + '''bool
radv_ps5_memory_use(struct radv_ps5_memory *memory, uint64_t offset, uint64_t size, int delta)
{
   if (size == 0 || offset >= memory->bytes)'''),
        ('''   uint64_t returned = 0, held_back = 0;
   for (uint32_t i = 0; i < radv_ps5_trim_count && zero >= 0 && (!want || returned < want); ++i) {
      struct radv_ps5_memory *memory = radv_ps5_trim_tracked[i];
      const uint64_t granules = memory->bytes / RADV_PS5_LARGE_BYTES;
''',
         '''   uint64_t returned = 0, held_back = 0;
   uint32_t sparse_kept = 0; /* ''' + MARK + ''' */
   for (uint32_t i = 0; i < radv_ps5_trim_count && zero >= 0 && (!want || returned < want); ++i) {
      struct radv_ps5_memory *memory = radv_ps5_trim_tracked[i];
      if (memory->sparse_backing) {
         ++sparse_kept;
         continue;
      }
      const uint64_t granules = memory->bytes / RADV_PS5_LARGE_BYTES;
'''),
        ('''   radv_ps5_trim_returned += returned;
''',
         '''   radv_ps5_trim_returned += returned;
   if (sparse_kept) {
      static uint32_t sparse_told;
      if (sparse_told++ < 16 || sparse_told % 64 == 0)
         fprintf(stderr, "EDEN_DEFER_FREE radv trim: kept %u sparse-backing objects (trim call %u with any)\\n",
                 sparse_kept, sparse_told);
   }
'''),
        ('''uint64_t
radv_ps5_eden_trim(uint64_t want)
{
   (void)want;
   return 0;
}
''',
         '''uint64_t
radv_ps5_eden_trim(uint64_t want)
{
   (void)want;
   return 0;
}

void
radv_ps5_memory_sparse_backing(struct radv_ps5_memory *memory)
{
   memory->sparse_backing = true; /* ''' + MARK + ''' (host build: nothing is trimmed) */
}
'''),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_bo.c': [(
        '''   const bool done = bo ? radv_ps5_vrange_bind(''',
        '''   /* ''' + MARK + ''': marked before the range maps it. */
   if (bo)
      radv_ps5_memory_sparse_backing(&radv_ps5_bo(bo)->memory);
   const bool done = bo ? radv_ps5_vrange_bind(''')],
}

changed = False
for relative, replacements in edits.items():
    path = source / relative
    text = path.read_text()
    if MARK in text:
        continue
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f'{relative}: pinned anchor changed ({text.count(old)}x): {old.strip().splitlines()[0]!r}')
        text = text.replace(old, new)
    path.write_text(text)
    changed = True
if changed:
    receipt = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if source.resolve() == (root.parent / 'mihawk-vulkan-review/.deps/work/radv-src').resolve() and receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print('RADV sparse backing: memory a sparse range maps is never trimmed')
