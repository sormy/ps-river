// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cache.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
#endif

namespace Pkg {

namespace {

// The map: this header, then a byte for each chunk (1: here).
constexpr u32 MapMagic = 0x43474b50; // "PKGC"
constexpr u32 MapVersion = 1;
constexpr std::size_t MapHeaderSize = 32;
// Chunks fetched between two writes of the map.
constexpr u32 PersistEvery = 64;

u64 Fnv1a(std::string_view text) {
    u64 hash = 0xcbf29ce484222325;
    for (const char c : text) {
        hash = (hash ^ static_cast<u8>(c)) * 0x100000001b3;
    }
    return hash;
}

std::array<std::byte, MapHeaderSize> MapHeader(u64 size, std::string_view key) {
    std::array<std::byte, MapHeaderSize> header{};
    const auto put = [&](std::size_t at, u64 value, std::size_t bytes) {
        for (std::size_t i = 0; i < bytes; ++i) {
            header[at + i] = static_cast<std::byte>(value >> (8 * i));
        }
    };
    put(0, MapMagic, 4);
    put(4, MapVersion, 4);
    put(8, CachedSource::ChunkSize, 4);
    put(16, size, 8);
    put(24, Fnv1a(key), 8);
    return header;
}

[[noreturn]] void Fail(const std::string& what) {
#ifdef _WIN32
    throw std::runtime_error(what + " (error " + std::to_string(::GetLastError()) + ")");
#else
    throw std::runtime_error(what + ": " + std::strerror(errno));
#endif
}

} // namespace

bool OnNetworkShare(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring root = path.root_path().wstring();
    return root.starts_with(L"\\\\") || ::GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
#elif defined(__linux__)
    constexpr long Cifs = 0xff534d42, Smb2 = 0xfe534d42, Smb = 0x517b, Nfs = 0x6969;
    struct statfs info{};
    if (::statfs(path.c_str(), &info) != 0) {
        return false;
    }
    const long type = static_cast<long>(info.f_type);
    return type == Cifs || type == Smb2 || type == Smb || type == Nfs;
#elif defined(__APPLE__)
    struct statfs info{};
    if (::statfs(path.c_str(), &info) != 0) {
        return false;
    }
    const std::string_view type{info.f_fstypename};
    return type == "smbfs" || type == "nfs" || type == "afpfs" || type == "webdav";
#else
    return false;
#endif
}

void TrimCaches(const std::filesystem::path& folder, u64 limit, u64 room,
                std::span<const std::filesystem::path> keep) {
    struct Cache {
        std::filesystem::path data;
        std::filesystem::file_time_type used;
        u64 size;
    };
    std::vector<Cache> caches;
    u64 total = room;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const auto& map = entry.path();
        if (map.extension() != ".map") {
            continue;
        }
        auto data = map;
        data.replace_extension();
        const u64 size = std::filesystem::file_size(data, error);
        if (error) {
            continue;
        }
        total += size;
        if (std::ranges::find(keep, data) == keep.end()) {
            caches.push_back({data, std::filesystem::last_write_time(map, error), size});
        }
    }
    std::ranges::sort(caches, {}, &Cache::used);
    for (const Cache& cache : caches) {
        if (total <= limit) {
            break;
        }
        std::filesystem::remove(cache.data, error);
        std::filesystem::remove(std::filesystem::path{cache.data} += ".map", error);
        total -= cache.size;
    }
}

#ifdef _WIN32

LocalFile::LocalFile(const std::filesystem::path& path)
    : handle{::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)} {
    if (handle == INVALID_HANDLE_VALUE) {
        Fail(path.string());
    }
}

LocalFile::~LocalFile() {
    ::CloseHandle(handle);
}

u64 LocalFile::Size() const {
    LARGE_INTEGER length{};
    if (!::GetFileSizeEx(handle, &length)) {
        Fail("no size");
    }
    return static_cast<u64>(length.QuadPart);
}

void LocalFile::Resize(u64 size) const {
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFileInformationByHandle(handle, FileEndOfFileInfo, &end, sizeof(end))) {
        Fail("could not size a cache file");
    }
}

void LocalFile::ReadAt(u64 offset, std::span<std::byte> out) const {
    while (!out.empty()) {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        const auto count = static_cast<DWORD>(std::min<std::size_t>(out.size(), 1u << 30));
        if (!::ReadFile(handle, out.data(), count, &got, &at) || got == 0) {
            Fail("could not read a cache file");
        }
        out = out.subspan(got);
        offset += got;
    }
}

void LocalFile::WriteAt(u64 offset, std::span<const std::byte> data) const {
    while (!data.empty()) {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD put = 0;
        const auto count = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30));
        if (!::WriteFile(handle, data.data(), count, &put, &at) || put == 0) {
            Fail("could not write a cache file");
        }
        data = data.subspan(put);
        offset += put;
    }
}

void LocalFile::Sync() const {
    ::FlushFileBuffers(handle);
}

#else

LocalFile::LocalFile(const std::filesystem::path& path)
    : fd{::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644)} {
    if (fd < 0) {
        Fail(path.string());
    }
}

LocalFile::~LocalFile() {
    ::close(fd);
}

u64 LocalFile::Size() const {
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        Fail("no size");
    }
    return static_cast<u64>(info.st_size);
}

void LocalFile::Resize(u64 size) const {
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        Fail("could not size a cache file");
    }
}

void LocalFile::ReadAt(u64 offset, std::span<std::byte> out) const {
    while (!out.empty()) {
        const ssize_t got = ::pread(fd, out.data(), out.size(), static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            Fail("could not read a cache file");
        }
        out = out.subspan(static_cast<std::size_t>(got));
        offset += static_cast<u64>(got);
    }
}

void LocalFile::WriteAt(u64 offset, std::span<const std::byte> data) const {
    while (!data.empty()) {
        const ssize_t put = ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(offset));
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            Fail("could not write a cache file");
        }
        data = data.subspan(static_cast<std::size_t>(put));
        offset += static_cast<u64>(put);
    }
}

void LocalFile::Sync() const {
#ifdef __APPLE__
    ::fsync(fd);
#else
    ::fdatasync(fd);
#endif
}

#endif

CachedSource::CachedSource(SourcePtr remote_, const std::filesystem::path& cache,
                           std::string_view key)
    : remote{std::move(remote_)}, size{remote->Size()},
      chunk_count{(size + ChunkSize - 1) / ChunkSize}, data{cache},
      map{std::filesystem::path{cache} += ".map"},
      present{std::make_unique<std::atomic<u8>[]>(chunk_count)} {
    const auto header = MapHeader(size, key);
    std::vector<std::byte> stored(MapHeaderSize + chunk_count);
    const bool same =
        map.Size() == stored.size() && data.Size() == size &&
        (map.ReadAt(0, stored), std::equal(header.begin(), header.end(), stored.begin()));
    if (!same) {
        // Made for another file (or never made): starts over.
        std::ranges::fill(stored, std::byte{0});
        std::ranges::copy(header, stored.begin());
        data.Resize(0);
        data.Resize(size);
        map.Resize(0);
        map.WriteAt(0, stored);
    }
    for (u64 c = 0; c < chunk_count; ++c) {
        present[c].store(stored[MapHeaderSize + c] == std::byte{1}, std::memory_order_relaxed);
    }
    // Used now, as far as TrimCaches goes.
    std::error_code error;
    std::filesystem::last_write_time(std::filesystem::path{cache} += ".map",
                                     std::filesystem::file_time_type::clock::now(), error);
}

CachedSource::~CachedSource() {
    try {
        Persist();
    } catch (...) {
        // Not written down: those chunks are fetched again next time.
    }
}

void CachedSource::Ensure(u64 chunk) const {
    if (present[chunk].load(std::memory_order_acquire) != 0) {
        return;
    }
    std::scoped_lock lock{fetching[chunk % fetching.size()]};
    if (present[chunk].load(std::memory_order_acquire) != 0) {
        return;
    }
    // This chunk and the missing ones right after it, in one read.
    u64 last = chunk + 1;
    while (last < chunk_count && last < chunk + ReadAhead &&
           present[last].load(std::memory_order_acquire) == 0) {
        ++last;
    }
    const u64 start = chunk * ChunkSize;
    const u64 end = std::min(last * ChunkSize, size);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end - start));
    remote->Read(start, bytes);
    data.WriteAt(start, bytes);
    for (u64 c = chunk; c < last; ++c) {
        present[c].store(1, std::memory_order_release);
    }
    if (unpersisted.fetch_add(static_cast<u32>(last - chunk)) + (last - chunk) >= PersistEvery) {
        Persist();
    }
}

void CachedSource::Persist() const {
    std::scoped_lock lock{persisting};
    unpersisted.store(0);
    std::vector<std::byte> marks(chunk_count);
    for (u64 c = 0; c < chunk_count; ++c) {
        marks[c] = static_cast<std::byte>(present[c].load(std::memory_order_acquire));
    }
    // The bytes first: a chunk is never marked as here before it is.
    data.Sync();
    map.WriteAt(MapHeaderSize, marks);
    map.Sync();
}

void CachedSource::Read(u64 offset, std::span<std::byte> out) const {
    CheckRange(offset, out.size());
    while (!out.empty()) {
        const u64 chunk = offset / ChunkSize;
        const auto count =
            static_cast<std::size_t>(std::min<u64>((chunk + 1) * ChunkSize - offset, out.size()));
        if (present[chunk].load(std::memory_order_acquire) == 0) {
            last_miss.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                            std::memory_order_relaxed);
        }
        Ensure(chunk);
        data.ReadAt(offset, out.first(count));
        out = out.subspan(count);
        offset += count;
    }
}

void CachedSource::Fill(std::stop_token stop) const {
    for (u64 chunk = 0; chunk < chunk_count && !stop.stop_requested(); ++chunk) {
        if (present[chunk].load(std::memory_order_acquire) != 0) {
            continue;
        }
        while (!stop.stop_requested() &&
               std::chrono::steady_clock::now() -
                       std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{
                           last_miss.load(std::memory_order_relaxed)}} <
                   FillAfterQuiet) {
            std::this_thread::sleep_for(FillAfterQuiet / 10);
        }
        if (!stop.stop_requested()) {
            Ensure(chunk);
        }
    }
    Persist();
}

u64 CachedSource::CachedBytes() const {
    u64 chunks = 0;
    for (u64 c = 0; c < chunk_count; ++c) {
        chunks += present[c].load(std::memory_order_relaxed);
    }
    return std::min(chunks * ChunkSize, size);
}

} // namespace Pkg
