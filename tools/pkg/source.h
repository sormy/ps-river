// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pkg {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

/// The data is not what its format says it is.
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Bytes read at any offset: a file, part of another source, or what a layer makes of one.
/// Reads are safe from several threads at once.
class Source {
public:
    virtual ~Source() = default;
    virtual u64 Size() const = 0;
    /// Fills `out` from `offset`; throws FormatError for a range past the end.
    virtual void Read(u64 offset, std::span<std::byte> out) const = 0;

    std::vector<std::byte> ReadBytes(u64 offset, std::size_t count) const;

protected:
    void CheckRange(u64 offset, std::size_t count) const;
};

using SourcePtr = std::shared_ptr<const Source>;

/// A file on the host, read at offsets (pread, or ReadFile with an offset on Windows).
class FileSource final : public Source {
public:
    explicit FileSource(const std::filesystem::path& path);
    ~FileSource() override;
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    u64 Size() const override {
        return size;
    }
    void Read(u64 offset, std::span<std::byte> out) const override;

private:
#ifdef _WIN32
    void* handle;
#else
    int fd;
#endif
    u64 size;
};

/// Bytes held in memory.
class MemorySource final : public Source {
public:
    explicit MemorySource(std::vector<std::byte> bytes) : bytes{std::move(bytes)} {}

    u64 Size() const override {
        return bytes.size();
    }
    void Read(u64 offset, std::span<std::byte> out) const override;

private:
    std::vector<std::byte> bytes;
};

/// `size` bytes of `base` from `offset`.
class Slice final : public Source {
public:
    Slice(SourcePtr base, u64 offset, u64 size);

    u64 Size() const override {
        return size;
    }
    void Read(u64 offset, std::span<std::byte> out) const override;

private:
    SourcePtr base;
    u64 offset;
    u64 size;
};

/// Whole blocks of `base`, in the order listed, as one run of bytes `size` long.
class Chunks final : public Source {
public:
    Chunks(SourcePtr base, u32 block_size, std::vector<u64> blocks, u64 size);

    u64 Size() const override {
        return size;
    }
    void Read(u64 offset, std::span<std::byte> out) const override;

private:
    SourcePtr base;
    u32 block_size;
    std::vector<u64> blocks;
    u64 size;
};

/// A source made a block at a time from another one (decrypted, decompressed). Each thread keeps
/// the blocks it made lately (reads tend to go on where the last one stopped), so readers never
/// wait on one another.
class BlockSource : public Source {
public:
    u64 Size() const final {
        return size;
    }
    void Read(u64 offset, std::span<std::byte> out) const final;

protected:
    BlockSource(u64 size, u32 block_size);
    /// Makes block `index` (its whole `block_size` bytes, past the end too) into `block`.
    virtual void Load(u64 index, std::span<std::byte> block) const = 0;

    u64 size;
    u32 block_size;

private:
    /// Tells this source's blocks apart in the threads' caches, for the program's whole life (an
    /// address may be another source's later).
    u64 id;
};

/// Big-endian and little-endian numbers in a byte buffer.
template <class T>
T LoadLe(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<u64>(bytes[at + i]) << (8 * i));
    }
    return value;
}

template <class T>
T LoadBe(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<T>((static_cast<u64>(value) << 8) | static_cast<u64>(bytes[at + i]));
    }
    return value;
}

} // namespace Pkg
