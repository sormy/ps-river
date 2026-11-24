// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// pspkg: reads PS4 packages built from a dump ("fake" packages, the all-zero passcode).
//   pspkg list GAME.pkg                  its content id, sce_sys entries, folders and files
//   pspkg ls GAME.pkg [FOLDER]           what a folder of the game holds (folders end in /)
//   pspkg extract GAME.pkg DIR [PATH...] the game as installed (sce_sys entries included) into
//                                        DIR; with PATHs, only those files and folders
//   pspkg cat GAME.pkg PATH              one game file to standard output
//   pspkg id GAME.pkg                    its content id
//   pspkg sfo GAME.pkg KEY               a param.sfo value (CATEGORY: gd a game, gp an update)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "entry_names.h"
#include "package.h"

namespace {

using namespace Pkg;
namespace fs = std::filesystem;

constexpr std::size_t CopyChunk = 1 << 20;
/// Extraction shares files out in pieces of at most this, so one large file does not hold up
/// the end.
constexpr u64 PieceSize = u64{64} << 20;

void Stream(const Source& source, std::ostream& out) {
    std::vector<std::byte> chunk(CopyChunk);
    for (u64 at = 0; at < source.Size();) {
        const std::size_t count =
            static_cast<std::size_t>(std::min<u64>(chunk.size(), source.Size() - at));
        source.Read(at, std::span{chunk}.first(count));
        out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(count));
        at += count;
    }
}

/// An open host file, closed when done with.
class HostFile {
public:
    HostFile(const fs::path& path, int flags) : fd{::open(path.c_str(), flags | O_CLOEXEC, 0644)} {
        if (fd < 0) {
            throw std::runtime_error(path.string() + ": " + std::strerror(errno));
        }
    }
    ~HostFile() {
        ::close(fd);
    }
    HostFile(const HostFile&) = delete;
    HostFile& operator=(const HostFile&) = delete;

    void Resize(u64 size) const {
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error(std::string{"could not size a file: "} + std::strerror(errno));
        }
    }
    void WriteAt(u64 offset, std::span<const std::byte> data) const {
        while (!data.empty()) {
            const ssize_t written =
                ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(offset));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::runtime_error(std::string{"could not write: "} + std::strerror(errno));
            }
            data = data.subspan(static_cast<std::size_t>(written));
            offset += static_cast<u64>(written);
        }
    }

private:
    int fd;
};

struct Piece {
    const Pfs::File* file;
    u64 offset;
    u64 size;
};

void WritePiece(const Pfs& game, const Piece& piece, const fs::path& path) {
    const SourcePtr source = game.Open(*piece.file);
    const HostFile out{path, O_WRONLY};
    std::vector<std::byte> chunk(CopyChunk);
    for (u64 at = piece.offset; at < piece.offset + piece.size;) {
        const auto count =
            static_cast<std::size_t>(std::min<u64>(chunk.size(), piece.offset + piece.size - at));
        source->Read(at, std::span{chunk}.first(count));
        out.WriteAt(at, std::span{chunk}.first(count));
        at += count;
    }
}

/// Threads extraction runs on: PKG_THREADS if set, else one a core.
unsigned Threads() {
    if (const char* text = std::getenv("PKG_THREADS")) {
        if (const int threads = std::atoi(text); threads > 0) {
            return static_cast<unsigned>(threads);
        }
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Runs `work(i)` for every i below `count` on Threads() threads; the first failure stops the
/// rest and is thrown.
template <class Work>
void InParallel(std::size_t count, const Work& work) {
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    {
        std::vector<std::jthread> workers;
        for (unsigned t = 0; t < Threads(); ++t) {
            workers.emplace_back([&] {
                for (std::size_t i; !failed && (i = next++) < count;) {
                    try {
                        work(i);
                    } catch (...) {
                        std::scoped_lock lock{failure_mutex};
                        if (!failure) {
                            failure = std::current_exception();
                        }
                        failed = true;
                    }
                }
            });
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

int List(const Package& package) {
    std::cout << "content id " << package.ContentId() << '\n';
    for (const auto& entry : package.Entries()) {
        std::printf("entry %#06x %10u %s %s\n", entry.id, entry.size,
                    entry.encrypted ? "encrypted" : "plain    ",
                    EntryPath(entry.id).value_or("-").c_str());
    }
    const Pfs game = package.OpenGame();
    const auto list_folder = [&](const auto& self, const std::string& folder) -> void {
        std::printf("folder %s\n", folder.empty() ? "." : folder.c_str());
        for (const auto& entry : game.List(folder)) {
            if (entry.directory) {
                self(self, folder.empty() ? entry.name : folder + '/' + entry.name);
            }
        }
    };
    list_folder(list_folder, {});
    u64 total = 0;
    for (const auto& file : game.Files()) {
        std::printf("file %12llu %s\n", static_cast<unsigned long long>(file.size),
                    file.path.c_str());
        total += file.size;
    }
    std::printf("%zu files, %llu bytes\n", game.Files().size(),
                static_cast<unsigned long long>(total));
    return 0;
}

int Cat(const Package& package, std::string_view path) {
    const Pfs game = package.OpenGame();
    const Pfs::File* file = game.Find(path);
    if (file == nullptr) {
        std::cerr << "pspkg: no " << path << " in the game\n";
        return 1;
    }
    Stream(*game.Open(*file), std::cout);
    return std::cout ? 0 : 1;
}

/// A value of a param.sfo (strings and 32-bit numbers), none if it has no such key.
std::optional<std::string> SfoValue(std::span<const std::byte> sfo, std::string_view key) {
    constexpr u32 Magic = 0x46535000; // "\0PSF"
    constexpr std::size_t HeaderSize = 0x14;
    constexpr std::size_t IndexSize = 0x10;
    constexpr u16 Number = 0x0404;
    if (sfo.size() < HeaderSize || LoadLe<u32>(sfo, 0) != Magic) {
        throw FormatError("not a param.sfo");
    }
    const u32 keys = LoadLe<u32>(sfo, 8);
    const u32 values = LoadLe<u32>(sfo, 0xc);
    const u32 count = LoadLe<u32>(sfo, 0x10);
    for (u32 i = 0; i < count; ++i) {
        const std::size_t index = HeaderSize + i * IndexSize;
        const std::size_t name_at = keys + LoadLe<u16>(sfo, index);
        const std::size_t value_at = values + LoadLe<u32>(sfo, index + 0xc);
        const u32 length = LoadLe<u32>(sfo, index + 4);
        if (index + IndexSize > sfo.size() || name_at >= sfo.size() ||
            value_at + length > sfo.size()) {
            throw FormatError("a param.sfo entry past its end");
        }
        const char* name = reinterpret_cast<const char*>(sfo.data() + name_at);
        if (std::string_view{name, strnlen(name, sfo.size() - name_at)} != key) {
            continue;
        }
        if (LoadLe<u16>(sfo, index + 2) == Number) {
            return std::to_string(LoadLe<u32>(sfo, value_at));
        }
        const char* text = reinterpret_cast<const char*>(sfo.data() + value_at);
        return std::string{text, strnlen(text, length)};
    }
    return std::nullopt;
}

int Sfo(const Package& package, std::string_view key) {
    const Pfs game = package.OpenGame();
    const Pfs::File* file = game.Find("sce_sys/param.sfo");
    if (file == nullptr) {
        std::cerr << "pspkg: no param.sfo\n";
        return 1;
    }
    const SourcePtr source = game.Open(*file);
    const auto value =
        SfoValue(source->ReadBytes(0, static_cast<std::size_t>(source->Size())), key);
    if (!value) {
        std::cerr << "pspkg: no " << key << " in param.sfo\n";
        return 1;
    }
    std::cout << *value << '\n';
    return 0;
}

int Ls(const Package& package, std::string_view folder) {
    const Pfs game = package.OpenGame();
    if (!game.IsDirectory(folder)) {
        std::cerr << "pspkg: no folder " << folder << " in the game\n";
        return 1;
    }
    for (const auto& entry : game.List(folder)) {
        if (entry.directory) {
            std::cout << entry.name << "/\n";
            continue;
        }
        const std::string path =
            folder.empty() ? entry.name : std::string{folder} + '/' + entry.name;
        std::printf("%-48s %12llu\n", entry.name.c_str(),
                    static_cast<unsigned long long>(game.Find(path)->size));
    }
    return 0;
}

/// Whether `path` is one of `wanted` or inside one (all, when none is).
bool Wanted(std::string_view path, const std::vector<std::string_view>& wanted) {
    return wanted.empty() || std::ranges::any_of(wanted, [&](std::string_view prefix) {
               while (prefix.ends_with('/')) {
                   prefix.remove_suffix(1);
               }
               return path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) &&
                                         path[prefix.size()] == '/');
           });
}

int Extract(const Package& package, const fs::path& target,
            const std::vector<std::string_view>& wanted) {
    const Pfs game = package.OpenGame();
    std::vector<Piece> pieces;
    std::size_t files = 0;
    for (const auto& file : game.Files()) {
        if (!Wanted(file.path, wanted)) {
            continue;
        }
        ++files;
        const fs::path path = target / file.path;
        fs::create_directories(path.parent_path());
        HostFile{path, O_WRONLY | O_CREAT | O_TRUNC}.Resize(file.size);
        for (u64 at = 0; at < file.size; at += PieceSize) {
            pieces.push_back({&file, at, std::min(PieceSize, file.size - at)});
        }
    }
    InParallel(pieces.size(),
               [&](std::size_t i) { WritePiece(game, pieces[i], target / pieces[i].file->path); });
    std::cout << files << " files into " << target.string() << '\n';
    return files == 0 && !wanted.empty() ? 1 : 0;
}

Package Open(std::string_view path) {
    return Package{std::make_shared<FileSource>(fs::path{path})};
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args{argv + 1, argv + argc};
    try {
        if (args.size() == 2 && args[0] == "list") {
            return List(Open(args[1]));
        }
        if (args.size() == 2 && args[0] == "id") {
            std::cout << Open(args[1]).ContentId() << '\n';
            return 0;
        }
        if (args.size() == 3 && args[0] == "sfo") {
            return Sfo(Open(args[1]), args[2]);
        }
        if (args.size() == 3 && args[0] == "cat") {
            return Cat(Open(args[1]), args[2]);
        }
        if ((args.size() == 2 || args.size() == 3) && args[0] == "ls") {
            return Ls(Open(args[1]), args.size() == 3 ? args[2] : std::string_view{});
        }
        if (args.size() >= 3 && args[0] == "extract") {
            return Extract(Open(args[1]), fs::path{args[2]}, {args.begin() + 3, args.end()});
        }
        std::cerr << "usage: pspkg list|id GAME.pkg | pspkg ls GAME.pkg [FOLDER]\n"
                     "       pspkg extract GAME.pkg DIR [PATH...]\n"
                     "       pspkg cat|sfo GAME.pkg PATH|KEY\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "pspkg: " << error.what() << '\n';
        return 1;
    }
}
