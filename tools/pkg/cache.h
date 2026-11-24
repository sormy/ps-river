// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string_view>

#include "source.h"

namespace Pkg {

/// Whether `path` is on a network share (SMB, NFS...), where every read crosses the network.
bool OnNetworkShare(const std::filesystem::path& path);

/// Deletes the caches in `folder` used longest ago (a cache counts as used when it is opened)
/// until they and `room` more bytes fit in `limit`; never those in `keep` (in use, or about to be).
void TrimCaches(const std::filesystem::path& folder, u64 limit, u64 room,
                std::span<const std::filesystem::path> keep);

/// A file on the host read and written at offsets.
class LocalFile {
public:
    explicit LocalFile(const std::filesystem::path& path);
    ~LocalFile();
    LocalFile(const LocalFile&) = delete;
    LocalFile& operator=(const LocalFile&) = delete;

    u64 Size() const;
    void Resize(u64 size) const;
    void ReadAt(u64 offset, std::span<std::byte> out) const;
    void WriteAt(u64 offset, std::span<const std::byte> data) const;
    /// Has what was written reach the disk.
    void Sync() const;

private:
#ifdef _WIN32
    void* handle;
#else
    int fd;
#endif
};

/// `remote` (a package on a network share, say) kept on local disk as it is read, in chunks:
/// a second read of a chunk, in this run or a later one, is a local one. A miss fetches the
/// chunks after it too (a title reads files front to back), and Fill() can fetch the rest in
/// the background. `key` tells the remote file apart (its name, size and time): a cache made
/// for another starts over. Reads are safe from several threads at once.
class CachedSource final : public Source {
public:
    static constexpr u32 ChunkSize = 1 << 20;
    static constexpr u32 ReadAhead = 4; // chunks fetched together on a miss
    /// Fill() fetches only once the title's reads have needed the remote this long ago: they
    /// would wait behind its fetches on a slow share.
    static constexpr std::chrono::milliseconds FillAfterQuiet{2000};

    CachedSource(SourcePtr remote, const std::filesystem::path& cache, std::string_view key);
    ~CachedSource() override;

    u64 Size() const override {
        return size;
    }
    void Read(u64 offset, std::span<std::byte> out) const override;
    /// Fetches every chunk not yet here, in order, while reads leave the remote quiet, until done
    /// or `stop` is asked for.
    void Fill(std::stop_token stop) const;
    /// How much of the remote file is here.
    u64 CachedBytes() const;

private:
    void Ensure(u64 chunk) const;
    /// Writes down which chunks are here, once their bytes are on the disk.
    void Persist() const;

    SourcePtr remote;
    u64 size;
    u64 chunk_count;
    LocalFile data;
    LocalFile map;
    std::unique_ptr<std::atomic<u8>[]> present;
    mutable std::array<std::mutex, 64> fetching; // by chunk, so different chunks fetch at once
    mutable std::mutex persisting;
    mutable std::atomic<u32> unpersisted{0};
    /// When a read last had to fetch (steady clock).
    mutable std::atomic<std::chrono::steady_clock::rep> last_miss{0};
};

} // namespace Pkg
