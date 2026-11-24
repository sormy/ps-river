// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pfs.h"

#include <algorithm>

namespace Pkg {

namespace {

constexpr u64 Version = 1;
constexpr u64 Magic = 20130315;
constexpr std::size_t HeaderSize = 0x400;
constexpr std::size_t SeedAt = 0x370;
constexpr std::size_t SeedSize = 16;

enum Mode : u16 {
    Signed = 0x1,
    Encrypted = 0x4,
};

// An unsigned inode lists block numbers; a signed one a signature before each.
constexpr std::size_t PlainInodeSize = 0xa8;
constexpr std::size_t SignedInodeSize = 0x2c8;
constexpr std::size_t BlocksAt = 0x64;
constexpr std::size_t DirectBlocks = 12;
constexpr std::size_t SignatureSize = 32;
constexpr std::size_t SignedRef = SignatureSize + sizeof(s32);

enum DirentType : s32 {
    FileEntry = 2,
    DirEntry = 3,
};
constexpr std::size_t DirentNameAt = 0x10;
constexpr std::string_view GameRoot = "uroot";
constexpr int MaxDepth = 64;

Digest CryptoKey(std::span<const std::byte> key, std::span<const std::byte> seed, u32 index) {
    std::vector<std::byte> message(sizeof(index));
    for (std::size_t i = 0; i < sizeof(index); ++i) {
        message[i] = static_cast<std::byte>(index >> (8 * i));
    }
    message.insert(message.end(), seed.begin(), seed.end());
    return HmacSha256(key, message);
}

} // namespace

XtsKeys PfsKeys(std::span<const std::byte> ekpfs, std::span<const std::byte> seed, bool new_crypt) {
    constexpr u32 EncryptionKeyIndex = 1;
    const Digest mixed = HmacSha256(ekpfs, seed);
    const Digest key =
        CryptoKey(new_crypt ? std::span<const std::byte>{mixed} : ekpfs, seed, EncryptionKeyIndex);
    XtsKeys keys;
    std::copy_n(key.begin(), keys.tweak.size(), keys.tweak.begin());
    std::copy_n(key.begin() + keys.tweak.size(), keys.data.size(), keys.data.begin());
    return keys;
}

Pfs::Pfs(SourcePtr source, std::optional<Digest> ekpfs, bool new_crypt) : image{std::move(source)} {
    const auto header = image->ReadBytes(0, HeaderSize);
    if (LoadLe<u64>(header, 0) != Version || LoadLe<u64>(header, 8) != Magic) {
        throw FormatError("not a PFS image");
    }
    const u16 mode = LoadLe<u16>(header, 0x1c);
    block_size = LoadLe<u32>(header, 0x20);
    is_signed = (mode & Signed) != 0;
    const u64 inode_count = LoadLe<u64>(header, 0x30);
    const u64 inode_blocks = LoadLe<u64>(header, 0x40);
    if (block_size < XtsSource::SectorSize || block_size % XtsSource::SectorSize != 0) {
        throw FormatError("a PFS image with an odd block size");
    }
    if ((mode & Encrypted) != 0) {
        if (!ekpfs) {
            throw FormatError("the PFS image is encrypted and no key was given");
        }
        const XtsKeys keys =
            PfsKeys(*ekpfs, std::span{header}.subspan(SeedAt, SeedSize), new_crypt);
        image = std::make_shared<XtsSource>(image, keys.data, keys.tweak,
                                            block_size / XtsSource::SectorSize);
    }
    Walk(ReadInodes(inode_count, inode_blocks), 0, {}, 0);
}

std::vector<Pfs::Inode> Pfs::ReadInodes(u64 count, u64 inode_blocks) const {
    const std::size_t size = is_signed ? SignedInodeSize : PlainInodeSize;
    const u64 per_block = block_size / size;
    if (count > inode_blocks * per_block) {
        throw FormatError("more inodes than their blocks hold");
    }
    std::vector<Inode> read;
    read.reserve(count);
    for (u64 block = 0; read.size() < count; ++block) {
        const auto bytes = image->ReadBytes((1 + block) * block_size, block_size);
        for (u64 i = 0; i < per_block && read.size() < count; ++i) {
            read.push_back(ParseInode(std::span{bytes}.subspan(i * size, size)));
        }
    }
    return read;
}

Pfs::Inode Pfs::ParseInode(std::span<const std::byte> bytes) const {
    Inode inode{.size = LoadLe<u64>(bytes, 8),
                .block_count = LoadLe<u32>(bytes, 0x60),
                .blocks = {},
                .single_indirect = 0,
                .double_indirect = 0};
    const std::size_t ref = is_signed ? SignedRef : sizeof(s32);
    const std::size_t number = is_signed ? SignatureSize : 0;
    const auto block = [&](std::size_t index) {
        return static_cast<u64>(LoadLe<u32>(bytes, BlocksAt + index * ref + number));
    };
    for (std::size_t i = 0; i < DirectBlocks && i < inode.block_count; ++i) {
        inode.blocks.push_back(block(i));
    }
    inode.single_indirect = block(DirectBlocks);
    inode.double_indirect = block(DirectBlocks + 1);
    return inode;
}

std::vector<u64> Pfs::FileBlocks(const Inode& inode) const {
    // An unsigned image keeps a file in one run; a signed one may scatter it, listing the rest
    // of its blocks in indirect blocks (signature, block number) and in blocks of those.
    if (!is_signed || inode.block_count <= 1) {
        return {};
    }
    std::vector<u64> blocks = inode.blocks;
    const u64 per_block = block_size / SignedRef;
    const auto read_refs = [&](u64 table, u64 count) {
        const auto bytes = image->ReadBytes(table * block_size, block_size);
        for (u64 i = 0; i < count; ++i) {
            blocks.push_back(LoadLe<u32>(bytes, i * SignedRef + SignatureSize));
        }
    };
    if (blocks.size() < inode.block_count) {
        read_refs(inode.single_indirect,
                  std::min<u64>(inode.block_count - blocks.size(), per_block));
    }
    if (blocks.size() < inode.block_count) {
        const auto tables = image->ReadBytes(inode.double_indirect * block_size, block_size);
        for (u64 j = 0; blocks.size() < inode.block_count && j < per_block; ++j) {
            const u64 table = LoadLe<u32>(tables, j * SignedRef + SignatureSize);
            read_refs(table, std::min<u64>(inode.block_count - blocks.size(), per_block));
        }
    }
    if (blocks.size() != inode.block_count) {
        throw FormatError("a file's blocks are not all listed");
    }
    const bool one_run =
        std::ranges::adjacent_find(blocks, [](u64 a, u64 b) { return a + 1 != b; }) == blocks.end();
    return one_run ? std::vector<u64>{} : blocks;
}

void Pfs::Walk(const std::vector<Inode>& inodes, u32 number, const std::string& path, int depth) {
    if (number >= inodes.size() || depth > MaxDepth) {
        throw FormatError("a directory tree that does not hold together");
    }
    const Inode& directory = inodes[number];
    if (directory.blocks.empty()) {
        throw FormatError("a directory without blocks");
    }
    const bool super_root = depth == 0;
    for (u64 block = 0; block < directory.block_count; ++block) {
        const auto bytes =
            image->ReadBytes((directory.blocks.front() + block) * block_size, block_size);
        for (std::size_t at = 0; at + DirentNameAt <= bytes.size();) {
            const u32 child = LoadLe<u32>(bytes, at);
            const s32 type = LoadLe<s32>(bytes, at + 4);
            const u32 name_length = LoadLe<u32>(bytes, at + 8);
            const u32 entry_size = LoadLe<u32>(bytes, at + 0xc);
            if (entry_size == 0 || at + DirentNameAt + name_length > bytes.size()) {
                break;
            }
            const std::string name{reinterpret_cast<const char*>(bytes.data() + at + DirentNameAt),
                                   name_length};
            at += entry_size;
            if (super_root) {
                if (type == DirEntry && name == GameRoot) {
                    folders.try_emplace(std::string{});
                    Walk(inodes, child, {}, depth + 1);
                }
                continue;
            }
            const std::string child_path = path.empty() ? name : path + '/' + name;
            if (type == DirEntry || type == FileEntry) {
                folders[path].push_back({name, type == DirEntry});
            }
            if (type == DirEntry) {
                folders.try_emplace(child_path);
                Walk(inodes, child, child_path, depth + 1);
            } else if (type == FileEntry) {
                if (child >= inodes.size()) {
                    throw FormatError("a file entry without its inode");
                }
                const Inode& file = inodes[child];
                index.emplace(child_path, files.size());
                files.push_back({child_path, file.size, FileBlocks(file),
                                 file.blocks.empty() ? 0 : file.blocks.front(), nullptr});
            }
        }
    }
}

bool Pfs::IsDirectory(std::string_view path) const {
    return folders.contains(path);
}

const std::vector<Pfs::Entry>& Pfs::List(std::string_view path) const {
    static const std::vector<Entry> None;
    const auto found = folders.find(path);
    return found == folders.end() ? None : found->second;
}

const Pfs::File* Pfs::Find(std::string_view path) const {
    const auto found = index.find(path);
    return found == index.end() ? nullptr : &files[found->second];
}

void Pfs::Add(const std::string& path, SourcePtr data) {
    if (const auto found = index.find(path); found != index.end()) {
        File& file = files[found->second];
        file.size = data->Size();
        file.data = std::move(data);
        return;
    }
    std::string folder;
    for (std::size_t at = 0, slash; (slash = path.find('/', at)) != std::string::npos;
         at = slash + 1) {
        std::string inner = path.substr(0, slash);
        if (!folders.contains(inner)) {
            folders[folder].push_back({path.substr(at, slash - at), true});
            folders.try_emplace(inner);
        }
        folder = std::move(inner);
    }
    folders[folder].push_back({path.substr(path.rfind('/') + 1), false});
    index.emplace(path, files.size());
    files.push_back({path, data->Size(), {}, 0, std::move(data)});
}

SourcePtr Pfs::Open(const File& file) const {
    if (file.data) {
        return file.data;
    }
    if (!file.blocks.empty()) {
        return std::make_shared<Chunks>(image, block_size, file.blocks, file.size);
    }
    return std::make_shared<Slice>(image, file.start * block_size, file.size);
}

} // namespace Pkg
