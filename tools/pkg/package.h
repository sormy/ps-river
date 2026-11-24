// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "pfs.h"

namespace Pkg {

/// The key a passcode gives a package (its EKPFS at index 1, entry keys at others).
Digest PasscodeKey(std::string_view content_id, std::string_view passcode, u32 index);

/// A PS4 package (.pkg): its entries (the sce_sys files and the package's own records) and its
/// game image. Only packages built with a passcode open (a dump's "fake" package: all zeros).
class Package {
public:
    static constexpr std::string_view FakePasscode = "00000000000000000000000000000000";

    struct Entry {
        u32 id;
        u32 offset;
        u32 size;
        bool encrypted;
        u32 key_index;
        std::array<std::byte, 32> record; // as the entry table has it (keys are made from it)
    };

    explicit Package(SourcePtr file);

    const std::string& ContentId() const {
        return content_id;
    }
    const std::vector<Entry>& Entries() const {
        return entries;
    }
    std::vector<std::byte> ReadEntry(const Entry& entry,
                                     std::string_view passcode = FakePasscode) const;
    /// The game as installed: the image inside the package's own image, and in its sce_sys the
    /// entries a game keeps (param.sfo, icons, trophies...).
    Pfs OpenGame(std::string_view passcode = FakePasscode) const;

private:
    SourcePtr file;
    std::string content_id;
    std::vector<Entry> entries;
    u64 image_offset;
    u64 image_size;
    bool new_crypt;
};

} // namespace Pkg
