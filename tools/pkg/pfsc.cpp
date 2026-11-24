// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pfsc.h"

#include <algorithm>

#include <zlib.h>

namespace Pkg {

namespace {

struct Header {
    u32 block_size;
    u64 offsets_at;
    u64 data_length;
};

Header ReadHeader(const Source& base) {
    const auto bytes = base.ReadBytes(0, PfscSource::HeaderSize);
    if (LoadLe<u32>(bytes, 0) != PfscSource::Magic || LoadLe<u32>(bytes, 4) != 0) {
        throw FormatError("not a PFSC image");
    }
    const u32 block_size = LoadLe<u32>(bytes, 0xc);
    if (block_size == 0 || LoadLe<u64>(bytes, 0x10) != block_size) {
        throw FormatError("a PFSC image with block sizes that disagree");
    }
    return {block_size, LoadLe<u64>(bytes, 0x18), LoadLe<u64>(bytes, 0x28)};
}

/// The zlib stream's 2-byte header is skipped: its body is raw deflate.
constexpr std::size_t ZlibHeader = 2;

void Inflate(std::span<std::byte> in, std::span<std::byte> out) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("zlib could not start");
    }
    stream.next_in = reinterpret_cast<Bytef*>(in.data());
    stream.avail_in = static_cast<uInt>(in.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (result != Z_STREAM_END && result != Z_BUF_ERROR) {
        throw FormatError("a PFSC block that does not inflate");
    }
}

} // namespace

PfscSource::PfscSource(SourcePtr base_) : BlockSource{0, 1}, base{std::move(base_)} {
    const Header header = ReadHeader(*base);
    size = header.data_length;
    block_size = header.block_size;
    const u64 blocks = (size + block_size - 1) / block_size;
    const auto table =
        base->ReadBytes(header.offsets_at, static_cast<std::size_t>((blocks + 1) * sizeof(u64)));
    offsets.resize(blocks + 1);
    for (u64 i = 0; i <= blocks; ++i) {
        offsets[i] = LoadLe<u64>(table, i * sizeof(u64));
    }
}

void PfscSource::Load(u64 index, std::span<std::byte> block) const {
    const u64 start = offsets[index];
    const u64 stored = offsets[index + 1] - start;
    std::ranges::fill(block, std::byte{0});
    if (stored == block_size) {
        base->Read(start, block);
    } else if (stored < block_size && stored > ZlibHeader) {
        auto packed =
            base->ReadBytes(start + ZlibHeader, static_cast<std::size_t>(stored - ZlibHeader));
        Inflate(packed, block);
    }
    // Stored larger than a block: a block of zeros, as the format has it.
}

} // namespace Pkg
