// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <charconv>
#include <filesystem>
#ifdef PS5_NATIVE
#include "native_directory.h"
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>

namespace Eden {
// Read the ACI0 block at the offset declared by META, including offsets beyond 1 KiB.
inline std::uint64_t ExtractedTitleID(const std::string& npdm) {
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(npdm.c_str(), "rb"), std::fclose);
    unsigned char header[0x80]{};
    if (!file || std::fread(header, 1, sizeof(header), file.get()) != sizeof(header) ||
        std::memcmp(header, "META", 4) != 0) return 0;
    std::uint32_t offset{};
    std::memcpy(&offset, header + 0x70, sizeof(offset));
    unsigned char aci[0x18]{};
    if (offset < sizeof(header) || std::fseek(file.get(), offset, SEEK_SET) != 0 ||
        std::fread(aci, 1, sizeof(aci), file.get()) != sizeof(aci) ||
        std::memcmp(aci, "ACI0", 4) != 0) return 0;
    std::uint64_t title{};
    std::memcpy(&title, aci + 0x10, sizeof(title));
    return title;
}

// Keep the library key at the game root. A folder game is base/ plus updates/<version>/, each with
// its own exefs/ and romfs/; an update's folders are the whole game at that version (Star Fox
// 2.0.0: 14.9 GB of romfs against 13.7 GB in base; Donkey Kong Bananza 3.2.0: 77342 files against
// 75238). The newest complete update (exefs/main and romfs/) is what boots: base/ first meant every
// folder game ran its 1.0 code and data whatever update sat next to it (6 Nov: Star Fox 1.0 faulted
// at start, Smash missed 13.0.5), while the same games as NSP or NUP had their update applied.
// An update without a romfs/ folder is not taken over base/ (the core takes the romfs/ next to the
// exefs/ it boots). A complete extracted update can also be the only installed layer.
inline std::string ExtractedGameMain(const std::string& root) {
    struct stat info {};
    std::string newest;
    std::uint64_t newest_version{};
    bool newest_complete = false;
    const std::string updates = root + "/updates";
    if (stat(updates.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
        std::error_code error;
#ifdef PS5_NATIVE
        const auto entries = ReadNativeDirectory(updates, error);
#else
        const auto entries = std::filesystem::directory_iterator(updates, error);
#endif
        if (!error) {
            for (const auto& entry : entries) {
                const auto name = entry.path().filename().string();
                std::uint64_t version{};
                const auto parsed = std::from_chars(name.data(), name.data() + name.size(), version);
                if (name.empty() || parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) continue;
                const auto main = entry.path().string() + "/exefs/main";
                if (stat(main.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
                const auto romfs = entry.path().string() + "/romfs";
                const bool complete = stat(romfs.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
                if (newest.empty() || version > newest_version) {
                    newest = main;
                    newest_version = version;
                    newest_complete = complete;
                }
            }
        }
    }
    if (!newest.empty() && newest_complete) return newest;
    for (const char* suffix : {"/base/exefs/main", "/exefs/main"}) {
        const std::string path = root + suffix;
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) return path;
    }
    return newest;
}
}
