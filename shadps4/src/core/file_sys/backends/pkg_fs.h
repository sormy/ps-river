// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "core/file_format/pkg/cache.h"
#include "core/file_format/pkg/pfs.h"
#include "core/file_sys/ifile.h"

namespace Core::FileSys {

/// A file of a package's game image (see PkgBackend).
class PkgFile final : public IFile {
public:
    /// `times`: those of the package, which its files have as theirs.
    PkgFile(Pkg::SourcePtr data, std::filesystem::path package, const FileStat& times);

    s64 Read(void* dst, u64 size) override;
    s64 Write(const void* src, u64 size) override;
    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override;
    u64 Tell() const override;
    u64 Size() const override;
    bool Flush() override;
    bool IsOpen() const override;
    std::optional<std::filesystem::path> GetMetadataHostPath() const override {
        return package;
    }
    void Stat(FileStat& out) override {
        out = stat;
    }
    MmapPolicy GetMmapPolicy() const override {
        return MmapPolicy::Copy;
    }
    bool Map(u8* addr, u64 size, u64 offset, u32 raw_prot, const FileMapContext& ctx) override;

private:
    /// Reads what the file has of `out.size()` bytes from `offset`: how many that is.
    u64 ReadAt(u64 offset, std::span<std::byte> out) const;

    Pkg::SourcePtr data;
    std::filesystem::path package;
    FileStat stat;
    mutable std::mutex position_mutex;
    u64 position = 0;
};

class PkgDirectory final : public IDirectory {
public:
    explicit PkgDirectory(std::vector<DirEntry> entries);

    bool Next(DirEntry& out) override;
    void Rewind() override;

private:
    std::vector<DirEntry> entries;
    std::size_t next = 0;
};

/// A game run from its PlayStation package (.pkg) in place: the files of its game image and its
/// sce_sys entries. A package on a network share, or any with SHADPS4_PKG_CACHE=1, is read
/// through a local cache that fills in the background (Pkg::CachedSource).
class PkgBackend final : public IBackend {
public:
    /// Throws when `package` cannot be read as one.
    explicit PkgBackend(const std::filesystem::path& package);
    ~PkgBackend() override;

    bool Exists(std::string_view rel_path) override;
    bool IsDirectory(std::string_view rel_path) override;
    std::unique_ptr<IFile> Open(std::string_view rel_path,
                                Common::FS::FileAccessMode mode) override;
    std::unique_ptr<IDirectory> OpenDir(std::string_view rel_path) override;
    bool IsReadOnly() const override {
        return true;
    }
    [[nodiscard]] std::filesystem::path RootPath() const override {
        return package;
    }
    std::optional<std::vector<u8>> ReadFile(std::string_view rel_path) const override;

private:
    std::filesystem::path package;
    std::shared_ptr<const Pkg::CachedSource> cache; // none: read where it is
    Pkg::Pfs game;
    FileStat times;
    std::jthread fill; // fetching the rest into the cache
};

/// The backend of `package` for this run, opened the first time it is asked for: nullptr when
/// it is no package that can be read.
[[nodiscard]] std::shared_ptr<PkgBackend> OpenPackage(const std::filesystem::path& package);

} // namespace Core::FileSys
