#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Buffer object address log in the pinned console RADV (diagnostic, opt-in).

A GPU page fault on the PS5 names only a GPU address (klog "Protection fault addr(VA)"). The
winsys places every buffer object at its CPU mapping in the device-memory region
[0x40_0000_0000, ...), so the address belongs to a buffer object that is live, was freed, or never
was. With RADV_PS5_BO_LOG=1 (env.txt) every buffer object made or destroyed prints one stderr line
(RADV_BO + / -: id, GPU address, bytes asked, bytes mapped, 2 MiB granules, priority, flags,
sparse, time) so a fault address can be matched to its object afterwards. Without the variable it
costs one branch per buffer object. Applied after patch-radv-trim.py. Patches the build export,
never the Mesa checkout.
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
MARK = 'EglantinePS5 buffer object log'

path = source / 'src/amd/vulkan/winsys/ps5/radv_ps5_bo.c'
text = path.read_text()
if MARK not in text:
    anchor = 'static uint64_t radv_ps5_next_obj_id;\n'
    assert text.count(anchor) == 1, 'radv_ps5_next_obj_id changed'
    helper = anchor + '''
/* ''' + MARK + ''': RADV_PS5_BO_LOG=1 prints each buffer object's address range when it is
 * made and destroyed, so a GPU page fault address can be matched to its object. */
static int radv_ps5_bo_log_on = -1;

static void
radv_ps5_bo_log(char op, const struct radv_ps5_bo *bo, unsigned priority)
{
   if (radv_ps5_bo_log_on < 0) {
      const char *value = getenv("RADV_PS5_BO_LOG");
      radv_ps5_bo_log_on = value && value[0] == '1';
   }
   if (!radv_ps5_bo_log_on || !bo)
      return;
   fprintf(stderr,
           "RADV_BO %c id=%" PRIu64 " va=0x%" PRIx64 " size=0x%" PRIx64 " mapped=0x%" PRIx64
           " granules=%u prio=%u flags=0x%x sparse=%d imported=%d t=%" PRIu64 "\\n",
           op, (uint64_t)bo->base.obj_id, (uint64_t)bo->base.va, (uint64_t)bo->base.size,
           (uint64_t)bo->memory.bytes, (unsigned)bo->memory.granules, priority, (unsigned)bo->flags,
           bo->sparse ? 1 : 0, bo->imported ? 1 : 0, (uint64_t)(os_time_get_nano() / 1000000));
}
'''
    text = text.replace(anchor, helper)

    # Made: the sparse path and the ordinary path both end with *out_bo = &bo->base; return VK_SUCCESS.
    made = '''      bo->base.obj_id = p_atomic_inc_return(&radv_ps5_next_obj_id);
      *out_bo = &bo->base;
      return VK_SUCCESS;
'''
    assert text.count(made) == 1, 'sparse buffer creation changed'
    text = text.replace(made, '''      bo->base.obj_id = p_atomic_inc_return(&radv_ps5_next_obj_id);
      radv_ps5_bo_log('+', bo, priority);
      *out_bo = &bo->base;
      return VK_SUCCESS;
''')
    made = '''      p_atomic_add(&ws->allocated_gtt, bo->memory.bytes);

   *out_bo = &bo->base;
   return VK_SUCCESS;
}
'''
    assert text.count(made) == 1, 'buffer creation changed'
    text = text.replace(made, '''      p_atomic_add(&ws->allocated_gtt, bo->memory.bytes);

   radv_ps5_bo_log('+', bo, priority);
   *out_bo = &bo->base;
   return VK_SUCCESS;
}
''')
    gone = '''   struct radv_ps5_bo *const bo = radv_ps5_bo(base);
   if (!bo)
      return;
   if (bo->sparse) {
      radv_ps5_vrange_release(&bo->memory);
'''
    assert text.count(gone) == 1, 'buffer destruction changed'
    text = text.replace(gone, '''   struct radv_ps5_bo *const bo = radv_ps5_bo(base);
   if (!bo)
      return;
   radv_ps5_bo_log('-', bo, bo->report_priority);
   if (bo->sparse) {
      radv_ps5_vrange_release(&bo->memory);
''')
    bound = '''   if (!done)
      fprintf(stderr, "radv/ps5: sparse %s of %" PRIu64 " bytes at %p failed\\n", bo ? "bind" : "unbind", size,
              (void *)at);
'''
    assert text.count(bound) == 1, 'sparse binding changed'
    text = text.replace(bound, bound + '''   if (radv_ps5_bo_log_on > 0)
      fprintf(stderr, "RADV_BO b va=0x%" PRIx64 " size=0x%" PRIx64 " range=%" PRIu64 " from=%" PRIu64
              " at=0x%" PRIx64 " ok=%d\\n", (uint64_t)(uintptr_t)at, size, (uint64_t)parent->obj_id,
              bo ? (uint64_t)bo->obj_id : 0, bo ? (uint64_t)bo->va + bo_offset : 0, done ? 1 : 0);
''')
    path.write_text(text)
    provenance = source.parents[1] / 'native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print('RADV buffer object log: address ranges made/destroyed (RADV_PS5_BO_LOG=1)')
