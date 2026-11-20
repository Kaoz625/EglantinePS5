#!/usr/bin/env python3
"""Exercise the generated staging-cache reclamation with outstanding references."""
from pathlib import Path
import subprocess
import tempfile
from eglantine_source import EGLANTINE, source as eglantine_source  # the core, part of this repository

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
source = eglantine_source('src/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp')
start = source.index('void StagingBufferPool::ReleaseCompleted() {')
end = source.index('\nvoid StagingBufferPool::TickFrame()', start)
body = source[start:end]
code = r'''
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
#include "staging_buffers.h"
struct StagingBufferPool {
    struct StagingBuffer {
        unsigned index;
        std::uint64_t tick;
        std::span<std::byte> mapped_span{};
        unsigned log2_level = 24;
    };
    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        // Upstream (zortos nx2) keeps buffers handed out deferred apart from the entries until
        // FreeDeferred puts them back: ReleaseCompleted must never reach them.
        std::unordered_map<std::uint64_t, StagingBuffer> deferred;
        std::size_t iterate_index = 0, delete_index = 0;
    };
    using StagingBuffersCache = std::array<StagingBuffers, 3>;
    struct Scheduler {
        std::uint64_t completed = 10;
        bool IsFree(std::uint64_t tick) const { return tick <= completed; }
    } scheduler;
    StagingBuffersCache device_local_cache, upload_cache, download_cache;
    void ReleaseCompleted();
};
''' + body + r'''
int main() {
    StagingBufferPool pool;
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            // A deferred reference must survive even when its old tick is complete.
            level.entries = {{1, 9}, {3, 11}, {4, 10}};
            level.deferred = {{2, {2, 1}}};
            level.iterate_index = 3;
            level.delete_index = 2;
        }
    }
    pool.ReleaseCompleted();
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            assert(level.entries.size() == 1 && level.entries[0].index == 3);
            assert(level.deferred.size() == 1 && level.deferred.at(2).index == 2);
            assert(level.iterate_index <= level.entries.size());
            assert(level.delete_index < level.entries.size());
        }
    }
    pool.ReleaseCompleted(); // repeated pressure cannot release outstanding work
    pool.scheduler.completed = 11;
    pool.ReleaseCompleted();
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            assert(level.entries.empty());
            assert(level.deferred.size() == 1 && level.deferred.at(2).index == 2);
            // FreeDeferred: back into the entries with a completed tick.
            auto node = level.deferred.extract(2);
            node.mapped().tick = 11;
            level.entries.push_back(node.mapped());
        }
    }
    pool.ReleaseCompleted();
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            assert(level.entries.empty() && level.deferred.empty());
            assert(level.iterate_index == 0 && level.delete_index == 0);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='eglantine-staging-pressure-') as directory:
    source_path = Path(directory) / 'check.cpp'
    executable = Path(directory) / 'check'
    source_path.write_text(code)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(root / 'headless'),
                    str(source_path), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('Staging pressure: PASS completed-only reclamation, deferred/in-flight references, cache cursors and repeated cleanup')
