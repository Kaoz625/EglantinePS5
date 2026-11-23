#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Queue rings and scratch are freed only after the GPU is done with them.

When a submission needs more scratch (a shader with bigger spills) or bigger rings, RADV makes
new ones in radv_update_preamble_cs and destroys the old scratch, ring and descriptor buffers and
the old preamble command streams at once. On amdgpu the kernel keeps a destroyed buffer mapped
until the submissions that use it finish. The PS5 winsys unmaps and releases it immediately, so
work still on the GPU writes its spills to an unmapped page: Fast Fusion's race load, a compute
dispatch stuck on a "GPU Protection fault ... client:TCP access:Write Unmapped page" in RADV's
device region, right after the log showed the 10 MiB compute scratch replaced by a 24 MiB one
(RADV_PS5_BO_LOG). Before anything old is destroyed the queue now waits until every submission
made so far has finished (rare: only when the rings or scratch grow). RADV_PS5_PREAMBLE_WAIT=0
restores the immediate free. Patches the build export, never the Mesa checkout.
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
MARK = 'EglantinePS5 preamble wait'

path = source / 'src/amd/vulkan/radv_queue.c'
text = path.read_text()
if MARK not in text:
    anchor = '''      result = radv_finalize_cmd_stream(device, cs);
      if (result != VK_SUCCESS)
         goto fail;
   }

   if (queue->initial_full_flush_preamble_cs)
      radv_destroy_cmd_stream(device, queue->initial_full_flush_preamble_cs);
'''
    assert text.count(anchor) == 1, 'radv_update_preamble_cs changed'
    wait = '''   /* ''' + MARK + ''': the PS5 winsys frees a destroyed buffer at once (amdgpu keeps it until
    * the submissions using it finish), so wait for the GPU before the old scratch, rings,
    * descriptors and preambles go. */
   {
      const bool frees = (queue->scratch_bo && scratch_bo != queue->scratch_bo) ||
                         (queue->compute_scratch_bo && compute_scratch_bo != queue->compute_scratch_bo) ||
                         (queue->esgs_ring_bo && esgs_ring_bo != queue->esgs_ring_bo) ||
                         (queue->gsvs_ring_bo && gsvs_ring_bo != queue->gsvs_ring_bo) ||
                         (queue->descriptor_bo && descriptor_bo != queue->descriptor_bo) ||
                         queue->initial_full_flush_preamble_cs || queue->initial_preamble_cs ||
                         queue->continue_preamble_cs;
      const char *setting = getenv("RADV_PS5_PREAMBLE_WAIT");
      if (frees && !(setting && setting[0] == '0')) {
         for (unsigned i = 0; i < RADV_NUM_HW_CTX; i++) {
            if (device->hw_ctx[i]) {
               const int64_t start = os_time_get_nano();
               ws->ctx_wait_idle(device->hw_ctx[i], AMD_IP_GFX, 0);
               fprintf(stderr,
                       "RADV_PS5_PREAMBLE_WAIT waited %" PRId64 " us before freeing (scratch %" PRIu64
                       " -> %" PRIu64 " KiB, compute scratch %" PRIu64 " -> %" PRIu64 " KiB)\\n",
                       (os_time_get_nano() - start) / 1000,
                       queue->scratch_bo ? (uint64_t)queue->scratch_bo->size >> 10 : 0,
                       scratch_bo ? (uint64_t)scratch_bo->size >> 10 : 0,
                       queue->compute_scratch_bo ? (uint64_t)queue->compute_scratch_bo->size >> 10 : 0,
                       compute_scratch_bo ? (uint64_t)compute_scratch_bo->size >> 10 : 0);
               break;
            }
         }
      }
   }

'''
    head, tail = anchor.split('\n   }\n\n', 1)
    text = text.replace(anchor, head + '\n   }\n\n' + wait + tail)
    for include in ('#include <stdio.h>\n', '#include <stdlib.h>\n', '#include <inttypes.h>\n',
                    '#include "util/os_time.h"\n'):
        if include not in text:
            first = text.index('#include')
            text = text[:first] + include + text[first:]
    path.write_text(text)
    provenance = source.parents[1] / 'native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print('RADV preamble wait: old scratch/rings/preambles freed after the GPU is idle')
