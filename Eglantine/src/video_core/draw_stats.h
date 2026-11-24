// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// EDEN_DRAW_STATS=1 (24 Nov 2026, Kirby Air Riders black screen): every 5 s one stderr line of
// what the game sent and what was shown - draws (direct, indirect, dropped by a guard, not run
// because render_enable said no), clears, compute dispatches, the colour targets drawn to, and
// every presented layer (address, size, blending, CPU pixels or not, found in the texture cache or
// not, drawn to in the window or not). Off = one static bool test per call.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"

namespace VideoCommon::DrawStats {

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_DRAW_STATS") != nullptr;
    return enabled;
}

enum Counter : u32 {
    Draws,
    Indirect,
    Dropped,      // DrawManager guards (billions of vertices, indices past 4 GiB)
    Skipped,      // rasterizer guard (first + count past 16M)
    NotExecuted,  // render_enable / conditional rendering said no
    Clears,
    Dispatches,
    Presents,
    NumCounters,
};

struct Target {
    u64 address{}; // device address of colour target 0 (0 = none / not mapped)
    u32 width{};
    u32 height{};
    u32 format{};
    u64 draws{};
};

struct Layer {
    u64 address{};
    u32 width{};
    u32 height{};
    u32 blending{};
    bool cpu_pixels{};
    u64 hits{};    // texture cache had an image there
    u64 misses{};  // fell back to reading guest memory
};

struct State {
    std::array<std::atomic<u64>, NumCounters> counters{};
    std::mutex mutex;
    std::array<Target, 12> targets{};
    u64 other_target_draws{};
    std::array<Layer, 6> layers{};
    u64 other_layers{};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
};

inline State& Get() {
    static State state;
    return state;
}

inline void Count(Counter counter) {
    if (Enabled()) {
        Get().counters[counter].fetch_add(1, std::memory_order_relaxed);
    }
}

/// A draw to colour target 0 at `address` (device address).
inline void NoteTarget(u64 address, u32 width, u32 height, u32 format) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    for (auto& target : state.targets) {
        if (target.draws != 0 && target.address == address && target.width == width &&
            target.format == format) {
            ++target.draws;
            return;
        }
    }
    for (auto& target : state.targets) {
        if (target.draws == 0) {
            target = Target{address, width, height, format, 1};
            return;
        }
    }
    ++state.other_target_draws;
}

/// One presented layer; prints the window's line when 5 s have passed.
inline void NoteLayer(u64 address, u32 width, u32 height, u32 blending, bool cpu_pixels, bool hit) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    bool found = false;
    for (auto& layer : state.layers) {
        if ((layer.hits + layer.misses) != 0 && layer.address == address && layer.width == width &&
            layer.blending == blending && layer.cpu_pixels == cpu_pixels) {
            ++(hit ? layer.hits : layer.misses);
            found = true;
            break;
        }
    }
    if (!found) {
        for (auto& layer : state.layers) {
            if ((layer.hits + layer.misses) == 0) {
                layer = Layer{address, width, height, blending, cpu_pixels, hit ? 1u : 0u, hit ? 0u : 1u};
                found = true;
                break;
            }
        }
        if (!found) {
            ++state.other_layers;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - state.start).count();
    if (seconds < 5.0) {
        return;
    }
    u64 c[NumCounters];
    for (u32 i = 0; i < NumCounters; ++i) {
        c[i] = state.counters[i].exchange(0, std::memory_order_relaxed);
    }
    std::fprintf(stderr,
                 "EDEN_DRAW_STATS s=%.1f layer_draws=%llu draws=%llu indirect=%llu dropped=%llu skipped=%llu "
                 "not_executed=%llu clears=%llu dispatches=%llu other_target_draws=%llu other_layers=%llu\n",
                 seconds, (unsigned long long)c[Presents], (unsigned long long)c[Draws],
                 (unsigned long long)c[Indirect], (unsigned long long)c[Dropped],
                 (unsigned long long)c[Skipped], (unsigned long long)c[NotExecuted],
                 (unsigned long long)c[Clears], (unsigned long long)c[Dispatches],
                 (unsigned long long)state.other_target_draws, (unsigned long long)state.other_layers);
    for (const auto& layer : state.layers) {
        if ((layer.hits + layer.misses) == 0) {
            continue;
        }
        u64 drawn = 0;
        for (const auto& target : state.targets) {
            if (target.draws != 0 && target.address == layer.address) {
                drawn += target.draws;
            }
        }
        std::fprintf(stderr,
                     "EDEN_DRAW_STATS layer addr=%#llx %ux%u blend=%u cpu=%d hits=%llu misses=%llu "
                     "draws_to_it=%llu\n",
                     (unsigned long long)layer.address, layer.width, layer.height, layer.blending,
                     int(layer.cpu_pixels), (unsigned long long)layer.hits,
                     (unsigned long long)layer.misses, (unsigned long long)drawn);
    }
    for (const auto& target : state.targets) {
        if (target.draws == 0) {
            continue;
        }
        std::fprintf(stderr, "EDEN_DRAW_STATS target addr=%#llx %ux%u format=%#x draws=%llu\n",
                     (unsigned long long)target.address, target.width, target.height, target.format,
                     (unsigned long long)target.draws);
    }
    state.targets = {};
    state.layers = {};
    state.other_target_draws = 0;
    state.other_layers = 0;
    state.start = now;
}

} // namespace VideoCommon::DrawStats
