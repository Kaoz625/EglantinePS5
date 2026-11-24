// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// H47: a shader cache log read in record-aligned pieces parses the same as the whole file.

#include <algorithm>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/shader_cache_v2/cache_format.h"

namespace V2 = VideoCommon::ShaderCacheV2;

namespace {

std::vector<u8> MakeLog(std::mt19937& rng, size_t records, bool damage) {
    const auto header = V2::EncodeFileHeader(1234);
    std::vector<u8> bytes(header.begin(), header.end());
    std::uniform_int_distribution<size_t> small(0, 3000);
    for (size_t i = 0; i < records; ++i) {
        // Mostly small records, a few larger than a piece, some keys written again (superseded)
        const size_t size = i % 37 == 5 ? 70000 + small(rng) : small(rng);
        std::vector<u8> payload(size);
        for (u8& byte : payload) {
            byte = static_cast<u8>(rng());
        }
        V2::EncodeRecord(bytes, V2::RecordKind::Program, i % 50 == 7 ? 7 : i, payload);
        if (damage && i == records / 3) {
            // A stretch of garbage longer than a piece (damaged headers, then a scan)
            for (size_t g = 0; g < 50000; ++g) {
                bytes.push_back(static_cast<u8>(rng()));
            }
        }
        if (damage && i == records / 2) {
            bytes[bytes.size() - 20] ^= 0x5a; // a damaged payload
        }
    }
    if (damage) {
        bytes.resize(bytes.size() - 13); // a record cut off at the end
    }
    return bytes;
}

/// The loop of ReadLogFile, reading from memory
std::vector<std::vector<u8>> ReadInPieces(std::span<const u8> file, size_t piece_bytes) {
    std::vector<std::vector<u8>> pieces;
    std::vector<u8> carry;
    size_t done = 0;
    while (!file.empty()) {
        std::vector<u8> piece = std::move(carry);
        carry = {};
        const size_t want = std::min(piece_bytes, file.size() - done);
        piece.insert(piece.end(), file.begin() + done, file.begin() + done + want);
        const size_t piece_offset = done + want - piece.size();
        done += want;
        const bool last = done >= file.size();
        if (!last) {
            const u64 start = piece_offset == 0 ? V2::FileHeaderSize : 0;
            const u64 cut = V2::LogSegmentCut(piece, start);
            if (cut <= start) {
                carry = std::move(piece);
                continue;
            }
            carry.assign(piece.begin() + static_cast<std::ptrdiff_t>(cut), piece.end());
            piece.resize(cut);
        }
        pieces.push_back(std::move(piece));
        if (last) {
            break;
        }
    }
    return pieces;
}

void RequireSame(std::span<const u8> file, size_t piece_bytes) {
    const V2::ParsedLog whole = V2::ParseLog(file);
    const auto pieces = ReadInPieces(file, piece_bytes);
    std::vector<V2::LogSegment> segments;
    u64 offset = 0;
    for (const auto& piece : pieces) {
        segments.push_back({.bytes = piece, .offset = offset});
        offset += piece.size();
    }
    REQUIRE(offset == file.size());
    REQUIRE(pieces.size() > 1);
    const V2::ParsedLog split = V2::ParseLogSegments(segments);
    REQUIRE(split.header == whole.header);
    REQUIRE(split.live.size() == whole.live.size());
    for (size_t i = 0; i < whole.live.size(); ++i) {
        REQUIRE(split.live[i].key == whole.live[i].key);
        REQUIRE(split.live[i].offset == whole.live[i].offset);
        REQUIRE(std::ranges::equal(split.live[i].payload, whole.live[i].payload));
    }
    REQUIRE(split.stats.records == whole.stats.records);
    REQUIRE(split.stats.superseded == whole.stats.superseded);
    REQUIRE(split.stats.bad_headers == whole.stats.bad_headers);
    REQUIRE(split.stats.bad_payloads == whole.stats.bad_payloads);
    REQUIRE(split.stats.skipped_bytes == whole.stats.skipped_bytes);
    REQUIRE(split.stats.torn_tail_bytes == whole.stats.torn_tail_bytes);
    REQUIRE(split.stats.dead_bytes == whole.stats.dead_bytes);
    REQUIRE(split.stats.file_size == whole.stats.file_size);
}

} // Anonymous namespace

TEST_CASE("Shader cache v2: a log read in pieces parses as the whole file",
          "[video_core][shader_cache_v2]") {
    std::mt19937 rng{47};
    const auto clean = MakeLog(rng, 400, false);
    RequireSame(clean, 16384);
    RequireSame(clean, 4096); // most records longer than a piece
}

TEST_CASE("Shader cache v2: damage parses the same in pieces", "[video_core][shader_cache_v2]") {
    std::mt19937 rng{1147};
    const auto damaged = MakeLog(rng, 400, true);
    const V2::ParsedLog whole = V2::ParseLog(damaged);
    REQUIRE(whole.stats.bad_headers > 0);
    REQUIRE(whole.stats.bad_payloads > 0);
    REQUIRE(whole.stats.torn_tail_bytes > 0);
    REQUIRE(whole.stats.superseded > 0);
    RequireSame(damaged, 16384);
    RequireSame(damaged, 65536);
}
