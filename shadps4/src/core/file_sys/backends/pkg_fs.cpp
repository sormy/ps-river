// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/file_format/pkg/package.h"
#include "core/file_sys/backends/pkg_fs.h"

namespace Core::FileSys {

namespace {

/// Whether a package is read through a local cache: SHADPS4_PKG_CACHE=1 always, 0 never, else
/// when it is on a network share.
bool UsesCache(const std::filesystem::path& package) {
    if (const char* setting = std::getenv("SHADPS4_PKG_CACHE"); setting != nullptr) {
        return setting[0] == '1';
    }
    return Pkg::OnNetworkShare(package);
}

/// How much the package caches may take together: SHADPS4_PKG_CACHE_MAX_GB, else 100 GB.
u64 CacheLimit() {
    constexpr u64 GB = u64{1} << 30;
    constexpr u64 DefaultGb = 100;
    const char* setting = std::getenv("SHADPS4_PKG_CACHE_MAX_GB");
    const long long gb = setting != nullptr ? std::atoll(setting) : 0;
    return (gb > 0 ? static_cast<u64>(gb) : DefaultGb) * GB;
}

std::mutex caches_mutex;
// The caches this run reads through: kept when room is made for another.
std::vector<std::filesystem::path> caches_in_use;

std::shared_ptr<const Pkg::CachedSource> MakeCache(Pkg::SourcePtr file,
                                                   const std::filesystem::path& package) {
    const auto folder = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "packages";
    std::filesystem::create_directories(folder);
    const auto modified = std::filesystem::last_write_time(package).time_since_epoch().count();
    const std::string key =
        fmt::format("{} {} {}", package.filename().string(), file->Size(), modified);
    const auto cache = folder / fmt::format("{}-{:x}", package.stem().string(), file->Size());
    std::scoped_lock lock{caches_mutex};
    caches_in_use.push_back(cache);
    // Room for it unless it is there already (then counted among the others).
    const u64 room = std::filesystem::exists(cache) ? 0 : file->Size();
    Pkg::TrimCaches(folder, CacheLimit(), room, caches_in_use);
    return std::make_shared<const Pkg::CachedSource>(std::move(file), cache, key);
}

Pkg::Pfs OpenImage(const std::filesystem::path& package,
                   std::shared_ptr<const Pkg::CachedSource>& cache) {
    Pkg::SourcePtr file = std::make_shared<Pkg::FileSource>(package);
    if (UsesCache(package)) {
        cache = MakeCache(file, package);
        file = cache;
    }
    return Pkg::Package{file}.OpenGame();
}

FileStat TimesOf(const std::filesystem::path& package) {
    using namespace std::chrono;
    const auto since_epoch =
        file_clock::to_sys(std::filesystem::last_write_time(package)).time_since_epoch();
    const auto sec = duration_cast<seconds>(since_epoch);
    FileStat times;
    times.mtime_sec = times.atime_sec = times.ctime_sec = sec.count();
    times.mtime_nsec = times.atime_nsec = times.ctime_nsec =
        duration_cast<nanoseconds>(since_epoch - sec).count();
    return times;
}

} // namespace

PkgFile::PkgFile(Pkg::SourcePtr data_, std::filesystem::path package_, const FileStat& times)
    : data{std::move(data_)}, package{std::move(package_)}, stat{times} {
    stat.size = data->Size();
    stat.is_directory = false;
}

u64 PkgFile::ReadAt(u64 offset, std::span<std::byte> out) const {
    if (offset >= data->Size()) {
        return 0;
    }
    const auto count = static_cast<std::size_t>(std::min<u64>(out.size(), data->Size() - offset));
    data->Read(offset, out.first(count));
    return count;
}

s64 PkgFile::Read(void* dst, u64 size) {
    std::scoped_lock lock{position_mutex};
    const u64 read =
        ReadAt(position, {static_cast<std::byte*>(dst), static_cast<std::size_t>(size)});
    position += read;
    return static_cast<s64>(read);
}

s64 PkgFile::Write(const void*, u64) {
    return -1;
}

bool PkgFile::Seek(s64 offset, Common::FS::SeekOrigin origin) {
    std::scoped_lock lock{position_mutex};
    s64 base = 0;
    switch (origin) {
    case Common::FS::SeekOrigin::SetOrigin:
        break;
    case Common::FS::SeekOrigin::CurrentPosition:
        base = static_cast<s64>(position);
        break;
    case Common::FS::SeekOrigin::End:
        base = static_cast<s64>(data->Size());
        break;
    }
    if (base + offset < 0) {
        return false;
    }
    position = static_cast<u64>(base + offset);
    return true;
}

u64 PkgFile::Tell() const {
    std::scoped_lock lock{position_mutex};
    return position;
}

u64 PkgFile::Size() const {
    return data->Size();
}

bool PkgFile::Flush() {
    return true;
}

bool PkgFile::IsOpen() const {
    return true;
}

bool PkgFile::Map(u8* addr, u64 size, u64 offset, u32 raw_prot, const FileMapContext& ctx) {
    ctx.map_anonymous(addr, size);
    const u64 read = ReadAt(offset, {reinterpret_cast<std::byte*>(addr), size});
    // Past the file's end a mapping reads as zeros.
    std::memset(addr + read, 0, size - read);
    ctx.protect(addr, size, raw_prot);
    return true;
}

PkgDirectory::PkgDirectory(std::vector<DirEntry> entries_) : entries{std::move(entries_)} {}

bool PkgDirectory::Next(DirEntry& out) {
    if (next == entries.size()) {
        return false;
    }
    out = entries[next++];
    return true;
}

void PkgDirectory::Rewind() {
    next = 0;
}

PkgBackend::PkgBackend(const std::filesystem::path& package_)
    : package{package_}, game{OpenImage(package, cache)}, times{TimesOf(package)} {
    if (cache) {
        constexpr u64 MiB = 1 << 20;
        LOG_INFO(Common_Filesystem, "{} is read through a local cache: {} of {} MiB there",
                 package.string(), cache->CachedBytes() / MiB, cache->Size() / MiB);
        fill = std::jthread{[cache = cache, name = package.string()](std::stop_token stop) {
            try {
                cache->Fill(stop);
            } catch (const std::exception& error) {
                LOG_WARNING(Common_Filesystem, "{}: the rest not fetched into its cache: {}", name,
                            error.what());
            }
        }};
    }
}

PkgBackend::~PkgBackend() = default;

bool PkgBackend::Exists(std::string_view rel_path) {
    return game.IsDirectory(rel_path) || game.Find(rel_path) != nullptr;
}

bool PkgBackend::IsDirectory(std::string_view rel_path) {
    return game.IsDirectory(rel_path);
}

std::unique_ptr<IFile> PkgBackend::Open(std::string_view rel_path,
                                        Common::FS::FileAccessMode mode) {
    const Pkg::Pfs::File* file = game.Find(rel_path);
    if (file == nullptr || mode != Common::FS::FileAccessMode::Read) {
        return nullptr;
    }
    return std::make_unique<PkgFile>(game.Open(*file), package, times);
}

std::unique_ptr<IDirectory> PkgBackend::OpenDir(std::string_view rel_path) {
    if (!game.IsDirectory(rel_path)) {
        return nullptr;
    }
    std::vector<DirEntry> entries;
    for (const auto& entry : game.List(rel_path)) {
        u64 size = 0;
        if (!entry.directory) {
            const std::string path =
                rel_path.empty() ? entry.name : fmt::format("{}/{}", rel_path, entry.name);
            size = game.Find(path)->size;
        }
        entries.push_back({entry.name, entry.directory, size});
    }
    return std::make_unique<PkgDirectory>(std::move(entries));
}

std::optional<std::vector<u8>> PkgBackend::ReadFile(std::string_view rel_path) const {
    const Pkg::Pfs::File* file = game.Find(rel_path);
    if (file == nullptr) {
        return std::nullopt;
    }
    std::vector<u8> bytes(file->size);
    game.Open(*file)->Read(0, std::as_writable_bytes(std::span{bytes}));
    return bytes;
}

std::shared_ptr<PkgBackend> OpenPackage(const std::filesystem::path& package) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::shared_ptr<PkgBackend>> opened;
    std::scoped_lock lock{mutex};
    const auto key = std::filesystem::weakly_canonical(package);
    if (const auto found = opened.find(key); found != opened.end()) {
        return found->second;
    }
    try {
        auto backend = std::make_shared<PkgBackend>(key);
        opened.emplace(key, backend);
        return backend;
    } catch (const std::exception& error) {
        LOG_ERROR(Common_Filesystem, "{} cannot be read as a package: {}", package.string(),
                  error.what());
        return nullptr;
    }
}

} // namespace Core::FileSys
