// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "source.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Pkg {

namespace {

std::atomic<u64> next_block_source{1};

/// A block a thread made lately, of any block source (a layer reads the one below it, so a few
/// at a time are in use).
struct CachedBlock {
    u64 source = 0; // 0: none
    u64 index = 0;
    u64 used = 0;
    bool filling = false; // being made: the layers below must not take it
    std::vector<std::byte> bytes;
};

constexpr std::size_t BlocksPerThread = 8;
thread_local std::array<CachedBlock, BlocksPerThread> cached_blocks;
thread_local u64 cache_clock = 0;

} // namespace

std::vector<std::byte> Source::ReadBytes(u64 offset, std::size_t count) const {
    std::vector<std::byte> bytes(count);
    Read(offset, bytes);
    return bytes;
}

void Source::CheckRange(u64 offset, std::size_t count) const {
    if (offset > Size() || count > Size() - offset) {
        throw FormatError("read of " + std::to_string(count) + " bytes at " +
                          std::to_string(offset) + " is past the end (" + std::to_string(Size()) +
                          ")");
    }
}

#ifdef _WIN32

FileSource::FileSource(const std::filesystem::path& path)
    : handle{::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr)} {
    LARGE_INTEGER length{};
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(path.string() + ": cannot open (error " +
                                 std::to_string(::GetLastError()) + ")");
    }
    if (!::GetFileSizeEx(handle, &length)) {
        ::CloseHandle(handle);
        throw std::runtime_error(path.string() + ": no size");
    }
    size = static_cast<u64>(length.QuadPart);
}

FileSource::~FileSource() {
    ::CloseHandle(handle);
}

void FileSource::Read(u64 offset, std::span<std::byte> out) const {
    CheckRange(offset, out.size());
    constexpr DWORD MaxRead = 1u << 30;
    while (!out.empty()) {
        // An offset given with each read keeps reads from several threads apart.
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(out.size(), MaxRead));
        if (!::ReadFile(handle, out.data(), count, &got, &at) || got == 0) {
            throw std::runtime_error("read failed (error " + std::to_string(::GetLastError()) +
                                     ")");
        }
        out = out.subspan(got);
        offset += got;
    }
}

#else

FileSource::FileSource(const std::filesystem::path& path)
    : fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)} {
    if (fd < 0) {
        throw std::runtime_error(path.string() + ": " + std::strerror(errno));
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::runtime_error(path.string() + ": " + std::strerror(error));
    }
    size = static_cast<u64>(info.st_size);
}

FileSource::~FileSource() {
    ::close(fd);
}

void FileSource::Read(u64 offset, std::span<std::byte> out) const {
    CheckRange(offset, out.size());
    while (!out.empty()) {
        const ssize_t got = ::pread(fd, out.data(), out.size(), static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            throw std::runtime_error(std::string{"read failed: "} +
                                     (got == 0 ? "end of file" : std::strerror(errno)));
        }
        out = out.subspan(static_cast<std::size_t>(got));
        offset += static_cast<u64>(got);
    }
}

#endif

void MemorySource::Read(u64 offset, std::span<std::byte> out) const {
    CheckRange(offset, out.size());
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
}

Slice::Slice(SourcePtr base, u64 offset, u64 size)
    : base{std::move(base)}, offset{offset}, size{size} {
    if (offset > this->base->Size() || size > this->base->Size() - offset) {
        throw FormatError("a part past the end of its source");
    }
}

void Slice::Read(u64 at, std::span<std::byte> out) const {
    CheckRange(at, out.size());
    base->Read(offset + at, out);
}

Chunks::Chunks(SourcePtr base, u32 block_size, std::vector<u64> blocks, u64 size)
    : base{std::move(base)}, block_size{block_size}, blocks{std::move(blocks)}, size{size} {
    if (size > static_cast<u64>(this->blocks.size()) * block_size) {
        throw FormatError("a file larger than its blocks");
    }
}

void Chunks::Read(u64 offset, std::span<std::byte> out) const {
    CheckRange(offset, out.size());
    while (!out.empty()) {
        const u64 index = offset / block_size;
        const u64 within = offset % block_size;
        const std::size_t count =
            static_cast<std::size_t>(std::min<u64>(block_size - within, out.size()));
        base->Read(blocks[index] * block_size + within, out.first(count));
        out = out.subspan(count);
        offset += count;
    }
}

BlockSource::BlockSource(u64 size, u32 block_size)
    : size{size}, block_size{block_size},
      id{next_block_source.fetch_add(1, std::memory_order_relaxed)} {}

void BlockSource::Read(u64 offset, std::span<std::byte> out) const {
    CheckRange(offset, out.size());
    while (!out.empty()) {
        const u64 index = offset / block_size;
        const u64 within = offset % block_size;
        auto block = std::ranges::find_if(cached_blocks, [&](const CachedBlock& cached) {
            return cached.source == id && cached.index == index;
        });
        if (block == cached_blocks.end()) {
            block = std::ranges::min_element(cached_blocks, {}, [](const CachedBlock& cached) {
                return cached.filling ? ~u64{0} : cached.used;
            });
            if (block->filling) {
                throw std::logic_error("more layers than cached blocks");
            }
            block->source = 0;
            block->filling = true;
            block->bytes.resize(block_size);
            try {
                Load(index, block->bytes);
            } catch (...) {
                block->filling = false;
                throw;
            }
            block->filling = false;
            block->source = id;
            block->index = index;
        }
        block->used = ++cache_clock;
        const std::size_t count =
            static_cast<std::size_t>(std::min<u64>(block_size - within, out.size()));
        std::copy_n(block->bytes.begin() + static_cast<std::ptrdiff_t>(within), count, out.begin());
        out = out.subspan(count);
        offset += count;
    }
}

} // namespace Pkg
