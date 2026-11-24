// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "package.h"

#include <algorithm>

#include "entry_names.h"
#include "pfsc.h"

namespace Pkg {

namespace {

constexpr u32 Magic = 0x7f434e54; // "\x7fCNT"
constexpr std::size_t HeaderSize = 0x1000;
constexpr std::size_t ContentIdAt = 0x40;
constexpr std::size_t ContentIdSize = 36;
constexpr std::size_t ContentIdPadded = 48;
constexpr std::size_t PassCodeSize = 32;
constexpr u64 NewCryptFlag = 0x2000000000000000;
constexpr std::size_t RecordSize = 32;
constexpr u32 EncryptedFlag = 0x80000000;
constexpr u32 KeyIndexMask = 0xf000;
constexpr u32 KeyIndexShift = 12;
constexpr u32 ImageKeyIndex = 1;
constexpr std::size_t AesBlock = 16;
constexpr std::string_view InnerImage = "pfs_image.dat";

std::span<const std::byte> AsBytes(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

} // namespace

Digest PasscodeKey(std::string_view content_id, std::string_view passcode, u32 index) {
    if (content_id.size() != ContentIdSize || passcode.size() != PassCodeSize) {
        throw FormatError("a content id of 36 characters and a passcode of 32 are needed");
    }
    std::array<std::byte, sizeof(index)> index_bytes;
    for (std::size_t i = 0; i < sizeof(index); ++i) {
        index_bytes[i] = static_cast<std::byte>(index >> (8 * (sizeof(index) - 1 - i)));
    }
    std::array<std::byte, ContentIdPadded> padded{};
    std::ranges::copy(AsBytes(content_id), padded.begin());
    std::vector<std::byte> data;
    for (const auto& part : {Sha256(index_bytes), Sha256(padded)}) {
        data.insert(data.end(), part.begin(), part.end());
    }
    const auto code = AsBytes(passcode);
    data.insert(data.end(), code.begin(), code.end());
    return Sha256(data);
}

Package::Package(SourcePtr source) : file{std::move(source)} {
    const auto header = file->ReadBytes(0, HeaderSize);
    if (LoadBe<u32>(header, 0) != Magic) {
        throw FormatError("not a PS4 package");
    }
    const auto id = std::span{header}.subspan(ContentIdAt, ContentIdSize);
    content_id.assign(reinterpret_cast<const char*>(id.data()), id.size());
    new_crypt = (LoadBe<u64>(header, 0x408) & NewCryptFlag) != 0;
    image_offset = LoadBe<u64>(header, 0x410);
    image_size = LoadBe<u64>(header, 0x418);

    const u32 count = LoadBe<u32>(header, 0x10);
    const u32 table = LoadBe<u32>(header, 0x18);
    const auto records = file->ReadBytes(table, static_cast<std::size_t>(count) * RecordSize);
    for (u32 i = 0; i < count; ++i) {
        const auto record = std::span{records}.subspan(i * RecordSize, RecordSize);
        Entry entry{.id = LoadBe<u32>(record, 0),
                    .offset = LoadBe<u32>(record, 0x10),
                    .size = LoadBe<u32>(record, 0x14),
                    .encrypted = (LoadBe<u32>(record, 0x8) & EncryptedFlag) != 0,
                    .key_index = (LoadBe<u32>(record, 0xc) & KeyIndexMask) >> KeyIndexShift,
                    .record = {}};
        std::ranges::copy(record, entry.record.begin());
        entries.push_back(entry);
    }
}

std::vector<std::byte> Package::ReadEntry(const Entry& entry, std::string_view passcode) const {
    if (!entry.encrypted) {
        return file->ReadBytes(entry.offset, entry.size);
    }
    auto data = file->ReadBytes(entry.offset, (entry.size + AesBlock - 1) / AesBlock * AesBlock);
    const Digest key = PasscodeKey(content_id, passcode, entry.key_index);
    std::vector<std::byte> seed{entry.record.begin(), entry.record.end()};
    seed.insert(seed.end(), key.begin(), key.end());
    const Digest iv_key = Sha256(seed);
    Key128 iv;
    Key128 aes_key;
    std::copy_n(iv_key.begin(), iv.size(), iv.begin());
    std::copy_n(iv_key.begin() + iv.size(), aes_key.size(), aes_key.begin());
    AesCbcDecrypt(aes_key, iv, data);
    data.resize(entry.size);
    return data;
}

Pfs Package::OpenGame(std::string_view passcode) const {
    const Pfs outer{std::make_shared<Slice>(file, image_offset, image_size),
                    PasscodeKey(content_id, passcode, ImageKeyIndex), new_crypt};
    const Pfs::File* inner = outer.Find(InnerImage);
    if (inner == nullptr) {
        throw FormatError("the package's image has no " + std::string{InnerImage});
    }
    Pfs game{std::make_shared<PfscSource>(outer.Open(*inner))};
    for (const Entry& entry : entries) {
        if (const auto path = InstalledEntryPath(entry.id)) {
            game.Add("sce_sys/" + *path,
                     entry.encrypted
                         ? SourcePtr{std::make_shared<MemorySource>(ReadEntry(entry, passcode))}
                         : std::make_shared<Slice>(file, entry.offset, entry.size));
        }
    }
    return game;
}

} // namespace Pkg
