// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "entry_names.h"

#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

namespace Pkg {

namespace {

constexpr std::array<std::pair<u32, std::string_view>, 31> Named{{
    {0x400, "license.dat"},
    {0x401, "license.info"},
    {0x402, "nptitle.dat"},
    {0x403, "npbind.dat"},
    {0x404, "selfinfo.dat"},
    {0x406, "imageinfo.dat"},
    {0x407, "target-deltainfo.dat"},
    {0x408, "origin-deltainfo.dat"},
    {0x409, "psreserved.dat"},
    {0x1000, "param.sfo"},
    {0x1001, "playgo-chunk.dat"},
    {0x1002, "playgo-chunk.sha"},
    {0x1003, "playgo-manifest.xml"},
    {0x1004, "pronunciation.xml"},
    {0x1005, "pronunciation.sig"},
    {0x1006, "pic1.png"},
    {0x1007, "pubtoolinfo.dat"},
    {0x1008, "app/playgo-chunk.dat"},
    {0x1009, "app/playgo-chunk.sha"},
    {0x100a, "app/playgo-manifest.xml"},
    {0x100b, "shareparam.json"},
    {0x100c, "shareoverlayimage.png"},
    {0x100d, "save_data.png"},
    {0x100e, "shareprivacyguardimage.png"},
    {0x1200, "icon0.png"},
    {0x1220, "pic0.png"},
    {0x1240, "snd0.at9"},
    {0x1260, "changeinfo/changeinfo.xml"},
    {0x1280, "icon0.dds"},
    {0x12a0, "pic0.dds"},
    {0x12c0, "pic1.dds"},
}};

/// Numbered series: `first` + n for n < `count`, named by `format` with `start` + n.
struct Series {
    u32 first;
    u32 count;
    u32 start;
    const char* format;
};

constexpr u32 Variants = 31; // _00 to _30
constexpr u32 Trophies = 100;
constexpr u32 Keymaps = 10;    // 001 to 010
constexpr u32 KeymapSets = 31; // keymap_rp/00 to /30

constexpr std::array<Series, 6> Numbered{{
    {0x1201, Variants, 0, "icon0_%02u.png"},
    {0x1241, Variants, 0, "pic1_%02u.png"},
    {0x1261, Variants, 0, "changeinfo/changeinfo_%02u.xml"},
    {0x1281, Variants, 0, "icon0_%02u.dds"},
    {0x12c1, Variants, 0, "pic1_%02u.dds"},
    {0x1400, Trophies, 0, "trophy/trophy%02u.trp"},
}};

constexpr u32 KeymapFirst = 0x1600;
constexpr u32 KeymapSetFirst = 0x1610;
constexpr u32 KeymapSetStride = 0x10;

std::string Format(const char* format, u32 number) {
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), format, number);
    return text.data();
}

} // namespace

std::optional<std::string> EntryPath(u32 id) {
    for (const auto& [named, path] : Named) {
        if (named == id) {
            return std::string{path};
        }
    }
    for (const Series& series : Numbered) {
        if (id >= series.first && id < series.first + series.count) {
            return Format(series.format, series.start + id - series.first);
        }
    }
    if (id >= KeymapFirst && id < KeymapFirst + Keymaps) {
        return Format("keymap_rp/%03u.png", id - KeymapFirst + 1);
    }
    if (id >= KeymapSetFirst && id < KeymapSetFirst + KeymapSets * KeymapSetStride) {
        const u32 set = (id - KeymapSetFirst) / KeymapSetStride;
        const u32 image = (id - KeymapSetFirst) % KeymapSetStride;
        if (image < Keymaps) {
            return Format("keymap_rp/%02u/", set) + Format("%03u.png", image + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::string> InstalledEntryPath(u32 id) {
    constexpr u32 LicenseDat = 0x400;
    constexpr u32 LicenseInfo = 0x401;
    auto path = EntryPath(id);
    if (!path || id == LicenseDat || id == LicenseInfo || path->ends_with(".dds")) {
        return std::nullopt;
    }
    return path;
}

} // namespace Pkg
