// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "common/fs/file.h"
#include "common/fs/file_lock.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/shader_cache_v2/cache_file.h"

namespace VideoCommon::ShaderCacheV2 {
namespace {

using Common::FS::FileAccessMode;
using Common::FS::FileShareFlag;
using Common::FS::FileType;
using Common::FS::IOFile;

/// How many bytes LogBuilder writes at once
constexpr size_t WriteChunk = 4 * 1024 * 1024;

u64 FileSize(const std::filesystem::path& file) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    return error ? 0 : static_cast<u64>(size);
}

/// Writes `bytes` to a temporary file, commits it to the disk and renames it over `file`
WriteResult ReplaceFile(const std::filesystem::path& file, std::span<const u8> bytes) {
    auto temporary = file;
    temporary += ".tmp";
    bool written{};
    {
        const IOFile target{temporary, FileAccessMode::Write, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        // The rename can reach the disk before the data: commit the data first.
        written = target.IsOpen() && target.WriteSpan(bytes) == bytes.size() && target.Commit();
    }
    std::error_code error;
    if (written) {
        std::filesystem::rename(temporary, file, error);
        if (!error) {
            return WriteResult::Written;
        }
    }
    LOG_ERROR(Common_Filesystem, "Failed to write shader cache {}: {}",
              Common::FS::PathToUTF8String(file),
              error ? error.message() : "the new file can't be written");
    std::filesystem::remove(temporary, error);
    return WriteResult::Failed;
}

} // Anonymous namespace

u64 UnixNow() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
}

LogFile ReadLogFile(const std::filesystem::path& file, const ForEach& for_each) {
    LogFile log;
    std::vector<LogSegment> segments;
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (stream.is_open()) {
        const std::streamoff size = stream.tellg();
        stream.seekg(0);
        // H47: in pieces of PieceBytes, each cut after the last record it holds whole; the rest
        // starts the next piece. (Also keeps every read below INT_MAX: MinGW's libstdc++ hands one
        // read to the CRT's _read, which aborts on a larger count.)
        static const u64 PieceBytes = [] {
            const char* value = std::getenv("EDEN_SC_READ_PIECE_MIB");
            const long mib = value ? std::strtol(value, nullptr, 10) : 32;
            return u64(mib >= 1 && mib <= 1024 ? mib : 32) << 20;
        }();
        u64 done = 0;
        std::vector<u8> carry;
        while (size > 0) {
            std::vector<u8> piece = std::move(carry);
            carry = {};
            const u64 piece_offset = done - piece.size();
            const u64 left = done < static_cast<u64>(size) ? static_cast<u64>(size) - done : 0;
            const u64 want = std::min<u64>(PieceBytes, left);
            const size_t kept = piece.size();
            piece.resize(kept + static_cast<size_t>(want));
            stream.read(reinterpret_cast<char*>(piece.data()) + kept,
                        static_cast<std::streamsize>(want));
            const u64 got = static_cast<u64>(stream.gcount());
            piece.resize(kept + static_cast<size_t>(got));
            done += got;
            // Cut short when the file shrank since (a compaction replaced it)
            const bool last = got != want || done >= static_cast<u64>(size);
            if (!last) {
                const u64 start = piece_offset == 0 ? FileHeaderSize : 0;
                const u64 cut = piece.size() < start ? start : LogSegmentCut(piece, start);
                if (cut <= start) {
                    // Not one whole record yet (a record or a damaged stretch longer than a
                    // piece): read on into the same piece
                    carry = std::move(piece);
                    continue;
                }
                carry.assign(piece.begin() + static_cast<std::ptrdiff_t>(cut), piece.end());
                piece.resize(static_cast<size_t>(cut));
            }
            if (!piece.empty()) {
                log.pieces.push_back(std::move(piece));
                segments.push_back({.bytes = log.pieces.back(), .offset = piece_offset});
            }
            if (last) {
                break;
            }
        }
    }
    // Spans into the pieces stay valid as log.pieces grows: a moved vector keeps its buffer.
    log.parsed = ParseLogSegments(segments, for_each);
    if (!log.pieces.empty()) {
        size_t largest = 0;
        for (const auto& piece : log.pieces) {
            largest = std::max(largest, piece.size());
        }
        std::fprintf(stderr, "EDEN_SC_READ pieces=%zu largest_mib=%.1f file_mib=%.1f (H47)\n",
                     log.pieces.size(), largest / 1048576.0,
                     log.parsed.stats.file_size / 1048576.0);
    }
    return log;
}

std::optional<std::vector<u8>> ReadRecordPayload(const std::filesystem::path& file,
                                                 RecordKind kind, u64 key, u64 offset,
                                                 u64 payload_size) {
    std::vector<u8> bytes(RecordHeaderSize + payload_size + RecordHashSize);
    std::ifstream stream(file, std::ios::binary);
    if (!stream.seekg(static_cast<std::streamoff>(offset)) ||
        !stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    const std::optional<LogRecord> record = ParseRecord(bytes, offset);
    if (!record || record->kind != kind || record->key != key ||
        record->payload.size() != payload_size) {
        return std::nullopt;
    }
    bytes.erase(bytes.begin(), bytes.begin() + RecordHeaderSize);
    bytes.resize(payload_size);
    return bytes;
}

WriteResult AppendToLog(const std::filesystem::path& file, std::span<const u8> records) {
    // Without locks on this file system (the lock is neither held nor busy) it goes on as the
    // other caches do there.
    const Common::FS::FileLock lock{PipelineCacheLockPath(file)};
    if (lock.Busy()) {
        return WriteResult::Busy;
    }
    const u64 size = FileSize(file);
    std::vector<u8> bytes;
    if (size < FileHeaderSize) {
        // New, or cut off within its header before any record was written
        const auto header = EncodeFileHeader(UnixNow());
        bytes.assign(header.begin(), header.end());
        bytes.insert(bytes.end(), records.begin(), records.end());
        const IOFile target{file, FileAccessMode::Write, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        if (target.IsOpen() && target.WriteSpan(std::span<const u8>{bytes}) == bytes.size() &&
            target.Flush()) {
            return WriteResult::Written;
        }
        LOG_ERROR(Common_Filesystem, "Failed to create shader cache {}",
                  Common::FS::PathToUTF8String(file));
        return WriteResult::Failed;
    }
    {
        std::array<u8, FileHeaderSize> header{};
        const IOFile source{file, FileAccessMode::Read, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        if (!source.IsOpen() || source.ReadSpan(std::span<u8>{header}) != header.size()) {
            return WriteResult::Failed;
        }
        switch (ParseLog(header).header) {
        case HeaderStatus::OtherVersion:
            return WriteResult::OtherVersion;
        case HeaderStatus::Damaged: {
            // The records check themselves and stay
            const IOFile target{file, FileAccessMode::ReadWrite,
                                FileType::BinaryFile, FileShareFlag::ShareReadWrite};
            const auto fresh = EncodeFileHeader(UnixNow());
            if (!target.IsOpen() || target.WriteSpan(std::span<const u8>{fresh}) != fresh.size() ||
                !target.Flush()) {
                return WriteResult::Failed;
            }
            break;
        }
        case HeaderStatus::Valid:
        case HeaderStatus::Empty:
            break;
        }
    }
    // A record cut off by a crash leaves the end off a record boundary.
    bytes.resize((RecordAlignment - size % RecordAlignment) % RecordAlignment);
    bytes.insert(bytes.end(), records.begin(), records.end());
    bool written{};
    {
        const IOFile target{file, FileAccessMode::Append, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        written = target.IsOpen() &&
                  target.WriteSpan(std::span<const u8>{bytes}) == bytes.size() && target.Flush();
    }
    if (written) {
        return WriteResult::Written;
    }
    // Cut off what was written (a full disk), so that the next records follow complete ones.
    std::error_code error;
    std::filesystem::resize_file(file, size, error);
    LOG_ERROR(Common_Filesystem, "Failed to append to shader cache {}",
              Common::FS::PathToUTF8String(file));
    return WriteResult::Failed;
}

WriteResult CompactLog(const std::filesystem::path& file, const CompactLimit& limit) {
    const Common::FS::FileLock lock{PipelineCacheLockPath(file)};
    if (!lock.Held()) {
        return WriteResult::Busy;
    }
    const LogFile log = ReadLogFile(file);
    if (log.parsed.header == HeaderStatus::Empty) {
        return WriteResult::Written;
    }
    if (log.parsed.header == HeaderStatus::OtherVersion) {
        return WriteResult::OtherVersion;
    }
    const auto header = EncodeFileHeader(log.parsed.created_unix != 0 ? log.parsed.created_unix
                                                                       : UnixNow());
    std::vector<u8> bytes(header.begin(), header.end());
    u64 live_bytes{};
    for (const LogRecord& record : log.parsed.live) {
        live_bytes += RecordSize(record.payload.size());
    }
    std::vector<const LogRecord*> kept;
    kept.reserve(log.parsed.live.size());
    for (const LogRecord& record : log.parsed.live) {
        kept.push_back(&record);
    }
    if (live_bytes > limit.max_bytes) {
        const auto rank = [&](const LogRecord* record) {
            return limit.rank ? limit.rank(*record) : 0;
        };
        std::ranges::stable_sort(kept, [&](const LogRecord* a, const LogRecord* b) {
            const u32 rank_a = rank(a);
            const u32 rank_b = rank(b);
            return rank_a != rank_b ? rank_a < rank_b : a->offset > b->offset;
        });
        u64 total{};
        size_t count{};
        while (count < kept.size() &&
               total + RecordSize(kept[count]->payload.size()) <= limit.target_bytes) {
            total += RecordSize(kept[count]->payload.size());
            ++count;
        }
        kept.resize(count);
        std::ranges::sort(kept, {}, &LogRecord::offset);
    }
    for (const LogRecord* record : kept) {
        EncodeRecord(bytes, record->kind, record->key, record->payload, record->flags);
    }
    return ReplaceFile(file, bytes);
}

// Out of line so that a caller does not need the complete type of the lock it holds
LogBuilder::LogBuilder() = default;

LogBuilder::~LogBuilder() {
    Discard();
}

WriteResult LogBuilder::Start(const std::filesystem::path& log) {
    file = log;
    lock = std::make_unique<Common::FS::FileLock>(PipelineCacheLockPath(file));
    const auto release = [this](WriteResult result) {
        lock.reset();
        file.clear();
        return result;
    };
    if (lock->Busy()) {
        return release(WriteResult::Busy);
    }
    std::error_code error;
    if (std::filesystem::exists(file, error) || error) {
        return release(WriteResult::Exists);
    }
    temporary = file;
    temporary += ".migrating";
    target = Common::FS::IOFile{temporary, FileAccessMode::Write, FileType::BinaryFile,
                                FileShareFlag::ShareReadWrite};
    if (!target.IsOpen()) {
        temporary.clear();
        return release(WriteResult::Failed);
    }
    const auto header = EncodeFileHeader(UnixNow());
    buffer.assign(header.begin(), header.end());
    buffer.reserve(WriteChunk);
    return WriteResult::Written;
}

bool LogBuilder::Append(RecordKind kind, u64 key, std::span<const u8> payload) {
    if (!written) {
        return false;
    }
    EncodeRecord(buffer, kind, key, payload);
    if (buffer.size() < WriteChunk) {
        return true;
    }
    if (target.WriteSpan(std::span<const u8>{buffer}) != buffer.size()) {
        written = false;
        return false;
    }
    buffer.clear();
    return true;
}

WriteResult LogBuilder::Commit() {
    if (!written || !target.IsOpen()) {
        Discard();
        return WriteResult::Failed;
    }
    if (!buffer.empty()) {
        const bool flushed = target.WriteSpan(std::span<const u8>{buffer}) == buffer.size();
        buffer.clear();
        if (!flushed) {
            Discard();
            return WriteResult::Failed;
        }
    }
    // The rename can reach the disk before the data: commit the data first.
    if (!target.Commit()) {
        Discard();
        return WriteResult::Failed;
    }
    target.Close();
    std::error_code error;
    if (std::filesystem::exists(file, error) || error) {
        Discard();
        return WriteResult::Exists;
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to write shader cache {}: {}",
                  Common::FS::PathToUTF8String(file), error.message());
        Discard();
        return WriteResult::Failed;
    }
    temporary.clear();
    lock.reset();
    return WriteResult::Written;
}

void LogBuilder::Discard() {
    if (temporary.empty()) {
        return;
    }
    target.Close();
    std::error_code error;
    std::filesystem::remove(temporary, error);
    temporary.clear();
    buffer.clear();
    lock.reset();
}

} // namespace VideoCommon::ShaderCacheV2
