#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RADV keeps no CPU copy of each shader binary unless a trace needs it.

radv_shader_binary_upload calloc'd shader->code (the shader's machine code, a copy of what goes to
GPU memory) for every shader, for the life of the shader. Kirby Air Riders imports ~6,400 compute
shaders and those copies held 172 MiB of the heap at its out-of-memory point (EDEN_HEAP_SITE
radv_shader.c:2713, H28). The copy is now made only when MESA_VK_TRACE or RADV_KEEP_SHADER_CODE=1 is
set (the SQTT layer reads it). radv_shader_serialize (pipeline binaries, the pipeline cache) also
copies it: without the copy it reads the code back from the shader's CPU mapping in GPU memory (as
upstream does for relocated binaries), and with no mapping it fails the blob instead of copying
from NULL (H29 first build: SIGSEGV in blob_write_bytes from vkCreatePipelineBinariesKHR, Kirby).
Ray-tracing re-upload also reads it; Eden makes no ray-tracing pipelines.
Patches the build export, never the Mesa checkout.
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


def patch(rel, old, new, what):
    path = source / rel
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f'Pinned {what} changed')
    path.write_text(text.replace(old, new))


patch('src/amd/vulkan/radv_shader.c', '''   shader->code = calloc(shader->code_size, 1);
   if (!shader->code) {
      radv_shader_unref(device, shader);
      return false;
   }
''', '''   /* EglantinePS5 (tools/patch-radv-shader-code.py): the CPU copy of the code is kept only when a
    * trace may run (MESA_VK_TRACE) or RADV_KEEP_SHADER_CODE=1; serialize reads the GPU mapping. */
   static int eden_keep_code = -1;
   if (eden_keep_code < 0) {
      const char *keep = getenv("RADV_KEEP_SHADER_CODE");
      eden_keep_code = getenv("MESA_VK_TRACE") != NULL || (keep != NULL && keep[0] == '1');
   }
   shader->code = NULL;
   if (eden_keep_code) {
      shader->code = calloc(shader->code_size, 1);
      if (!shader->code) {
         radv_shader_unref(device, shader);
         return false;
      }
   }
''', 'radv_shader_binary_upload code copy')

patch('src/amd/vulkan/radv_pipeline_cache.c', '''   blob_write_bytes(blob, shader->code, code_size);
''', '''   /* EglantinePS5 (tools/patch-radv-shader-code.py): no CPU copy kept -> the code is read back
    * from the shader's mapping in GPU memory; with none the blob fails instead of reading NULL. */
   if (shader->code) {
      blob_write_bytes(blob, shader->code, code_size);
   } else if (code_size && shader->alloc && shader->alloc->arena && shader->alloc->arena->ptr) {
      blob_write_bytes(blob, shader->alloc->arena->ptr + shader->alloc->offset, code_size);
   } else if (code_size) {
      blob->out_of_memory = true;
   }
''', 'radv_shader_serialize code write')
print('RADV shader code: CPU copy only for traces (MESA_VK_TRACE / RADV_KEEP_SHADER_CODE=1); '
      'serialize reads the GPU mapping')
