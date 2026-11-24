// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "crypto.h"

namespace Pkg {

struct XtsKeys {
    Key128 data;
    Key128 tweak;
};

/// The XTS keys of a PFS image from its EKPFS and seed; `new_crypt`: the package's newer scheme,
/// where the EKPFS is first mixed with the seed.
XtsKeys PfsKeys(std::span<const std::byte> ekpfs, std::span<const std::byte> seed, bool new_crypt);

/// A PFS image's files (those under its uroot, the game's root), each readable as a source.
class Pfs {
public:
    struct File {
        std::string path; // from the game's root, '/' between folders
        u64 size;
        std::vector<u64> blocks; // empty: one run from `start`
        u64 start;
        SourcePtr data; // a file added to the image's (Add): its bytes
    };

    /// `ekpfs`: the key of an encrypted image (none for a plain one).
    Pfs(SourcePtr image, std::optional<Digest> ekpfs = std::nullopt, bool new_crypt = false);

    /// A folder's entries: names, folders marked.
    struct Entry {
        std::string name;
        bool directory;
    };

    const std::vector<File>& Files() const {
        return files;
    }
    const File* Find(std::string_view path) const;
    /// Whether `path` is a folder ("" the game's root).
    bool IsDirectory(std::string_view path) const;
    /// The entries of folder `path`, none for a path that is not a folder.
    const std::vector<Entry>& List(std::string_view path) const;
    SourcePtr Open(const File& file) const;
    /// Adds a file the image does not hold (with the folders on its way), or replaces one.
    void Add(const std::string& path, SourcePtr data);

private:
    struct Inode {
        u64 size;
        u32 block_count;
        std::vector<u64> blocks; // as far as the inode itself lists them
        u64 single_indirect;
        u64 double_indirect;
    };

    std::vector<Inode> ReadInodes(u64 count, u64 inode_blocks) const;
    Inode ParseInode(std::span<const std::byte> bytes) const;
    std::vector<u64> FileBlocks(const Inode& inode) const;
    void Walk(const std::vector<Inode>& inodes, u32 number, const std::string& path, int depth);

    SourcePtr image;
    u32 block_size = 0;
    bool is_signed = false;
    std::vector<File> files;
    std::map<std::string, std::size_t, std::less<>> index; // a file's place in `files`
    std::map<std::string, std::vector<Entry>, std::less<>> folders;
};

} // namespace Pkg
