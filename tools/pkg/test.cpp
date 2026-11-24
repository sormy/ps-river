// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Checks the package reader's parts: entry names, keys, and each source layer on data made here.
// Whole packages are checked against a reference extraction by compare.sh.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include <unistd.h>

#include <openssl/evp.h>
#include <zlib.h>

#include "cache.h"
#include "entry_names.h"
#include "package.h"
#include "pfsc.h"

using namespace Pkg;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "ok    " : "FAIL  ") << what << '\n';
    failures += ok ? 0 : 1;
}

template <class F>
static bool Throws(F&& f) {
    try {
        f();
    } catch (const FormatError&) {
        return true;
    }
    return false;
}

static std::string Hex(std::span<const std::byte> bytes) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string text;
    for (const std::byte b : bytes) {
        text += Digits[std::to_integer<int>(b) >> 4];
        text += Digits[std::to_integer<int>(b) & 0xf];
    }
    return text;
}

static std::vector<std::byte> Pattern(std::size_t size, int salt) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 7 + static_cast<std::size_t>(salt)) & 0xff);
    }
    return bytes;
}

static void EntryNames() {
    const std::vector<std::pair<u32, std::optional<std::string>>> cases{
        {0x1000, "param.sfo"},
        {0x1008, "app/playgo-chunk.dat"},
        {0x0408, "origin-deltainfo.dat"},
        {0x1201, "icon0_00.png"},
        {0x121f, "icon0_30.png"},
        {0x1220, "pic0.png"},
        {0x1261, "changeinfo/changeinfo_00.xml"},
        {0x1400, "trophy/trophy00.trp"},
        {0x1463, "trophy/trophy99.trp"},
        {0x1600, "keymap_rp/001.png"},
        {0x1609, "keymap_rp/010.png"},
        {0x1610, "keymap_rp/00/001.png"},
        {0x17f9, "keymap_rp/30/010.png"},
        {0x160a, std::nullopt},
        {0x161a, std::nullopt},
        {0x0001, std::nullopt},
        {0x1464, std::nullopt},
    };
    for (const auto& [id, path] : cases) {
        Check(EntryPath(id) == path,
              "entry " + std::to_string(id) + " is " + path.value_or("unnamed"));
    }
}

static void Keys() {
    const std::string content_id = "EP9000-CUSA12392_00-PLATFORMERVR00EU";
    const Digest ekpfs = PasscodeKey(content_id, Package::FakePasscode, 1);
    Check(Hex(ekpfs) == "2a94599871330f038a423684ff88e46ea3883cf4792bb288d2c3e84f8bc2b816",
          "a passcode gives the package's EKPFS");
    Check(Throws([&] { PasscodeKey("short", Package::FakePasscode, 1); }),
          "a short content id is refused");
    Check(Throws([&] { PasscodeKey(content_id, "0", 1); }), "a short passcode is refused");

    std::array<std::byte, 16> seed;
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<std::byte>(i);
    }
    const XtsKeys old_keys = PfsKeys(ekpfs, seed, false);
    Check(Hex(old_keys.tweak) == "ae9fec2f6135aa927264709e3073782b" &&
              Hex(old_keys.data) == "1a855b2e39a69fe8798765edc49a2bcb",
          "an image's XTS keys from its EKPFS and seed");
    const XtsKeys new_keys = PfsKeys(ekpfs, seed, true);
    Check(Hex(new_keys.tweak) == "e3150e4eaf3065892961c4e321bfcc6f" &&
              Hex(new_keys.data) == "f47d0e8be4215bd31c63773dc34475b3",
          "the newer scheme mixes the seed in first");
}

static void Sources() {
    const auto bytes = Pattern(100, 1);
    const auto memory = std::make_shared<MemorySource>(bytes);
    Check(memory->ReadBytes(10, 5) ==
              std::vector<std::byte>(bytes.begin() + 10, bytes.begin() + 15),
          "memory reads what it holds");
    Check(Throws([&] { memory->ReadBytes(98, 3); }), "a read past the end is refused");
    const Slice slice{memory, 20, 30};
    Check(slice.ReadBytes(0, 30) == std::vector<std::byte>(bytes.begin() + 20, bytes.begin() + 50),
          "a slice reads its part");
    Check(Throws([&] { slice.ReadBytes(25, 6); }), "a slice ends where it ends");
    Check(Throws([&] { Slice(memory, 90, 20); }), "a slice past its source is refused");
    // Blocks 3, 1 of 10 bytes each: 15 bytes.
    const Chunks chunks{memory, 10, {3, 1}, 15};
    std::vector<std::byte> expected(bytes.begin() + 30, bytes.begin() + 40);
    expected.insert(expected.end(), bytes.begin() + 10, bytes.begin() + 15);
    Check(chunks.ReadBytes(0, 15) == expected,
          "chunks read their blocks in order, across their edge");
    Check(Throws([&] { Chunks(memory, 10, {0}, 11); }),
          "chunks larger than their blocks are refused");
}

/// A PFSC image of `data` in blocks of `block_size`: the first deflated, the second stored, the
/// third listed as larger than a block (zeros); `data` holds at most three blocks.
static std::vector<std::byte> MakePfsc(const std::vector<std::byte>& data, u32 block_size) {
    const u64 blocks = (data.size() + block_size - 1) / block_size;
    std::vector<std::byte> image(PfscSource::HeaderSize);
    const auto put = [&](std::size_t at, u64 value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            image[at + i] = static_cast<std::byte>(value >> (8 * i));
        }
    };
    put(0, PfscSource::Magic, 4);
    put(0xc, block_size, 4);
    put(0x10, block_size, 8);
    put(0x18, PfscSource::HeaderSize, 8);
    put(0x28, data.size(), 8);
    image.resize(PfscSource::HeaderSize + (blocks + 1) * sizeof(u64));
    std::vector<u64> offsets{image.size()};
    for (u64 b = 0; b < blocks; ++b) {
        std::vector<std::byte> block(block_size);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(b * block_size),
                    std::min<u64>(block_size, data.size() - b * block_size), block.begin());
        if (b == 0) {
            uLongf packed_size = compressBound(block_size);
            std::vector<std::byte> packed(packed_size);
            compress2(reinterpret_cast<Bytef*>(packed.data()), &packed_size,
                      reinterpret_cast<const Bytef*>(block.data()), block_size, Z_BEST_COMPRESSION);
            packed.resize(packed_size);
            image.insert(image.end(), packed.begin(), packed.end());
        } else if (b == 1) {
            image.insert(image.end(), block.begin(), block.end());
        } else {
            image.insert(image.end(), block_size + 1, std::byte{0xee});
        }
        offsets.push_back(image.size());
    }
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        put(PfscSource::HeaderSize + i * sizeof(u64), offsets[i], sizeof(u64));
    }
    return image;
}

static void Pfsc() {
    constexpr u32 BlockSize = 0x100;
    // The first block compressible (runs), the second not, the third zeros and short.
    std::vector<std::byte> data(BlockSize);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i / 32);
    }
    const auto varied = Pattern(BlockSize, 3);
    data.insert(data.end(), varied.begin(), varied.end());
    data.resize(BlockSize * 3 - 0x10, std::byte{0});
    const PfscSource pfsc{std::make_shared<MemorySource>(MakePfsc(data, BlockSize))};
    Check(pfsc.Size() == data.size(), "a PFSC image is as long as its data");
    Check(pfsc.ReadBytes(0, data.size()) == data, "deflated, stored and zero blocks read back");
    Check(pfsc.ReadBytes(BlockSize - 8, 16) ==
              std::vector<std::byte>(data.begin() + BlockSize - 8, data.begin() + BlockSize + 8),
          "a read across blocks");
    // Threads reading at once, each through its own cache, read what one reads alone.
    constexpr int Threads = 8;
    constexpr int ReadsEach = 2000;
    std::atomic<int> wrong{0};
    {
        std::vector<std::jthread> readers;
        for (int t = 0; t < Threads; ++t) {
            readers.emplace_back([&, t] {
                for (int i = 0; i < ReadsEach; ++i) {
                    const std::size_t at =
                        static_cast<std::size_t>(i * 37 + t * 101) % (data.size() - 40);
                    const std::size_t count = 1 + static_cast<std::size_t>(i + t) % 40;
                    if (pfsc.ReadBytes(at, count) !=
                        std::vector<std::byte>(data.begin() + static_cast<std::ptrdiff_t>(at),
                                               data.begin() +
                                                   static_cast<std::ptrdiff_t>(at + count))) {
                        ++wrong;
                    }
                }
            });
        }
    }
    Check(wrong == 0, "threads reading at once read what is there");
    auto broken = MakePfsc(data, BlockSize);
    broken[0] = std::byte{0};
    Check(Throws([&] { PfscSource{std::make_shared<MemorySource>(broken)}; }),
          "a PFSC image needs its magic");
}

static const Key128 DataKey{std::byte{1}, std::byte{2}, std::byte{3}};
static const Key128 TweakKey{std::byte{9}, std::byte{8}, std::byte{7}};

/// `bytes`' whole sectors from `first` on, encrypted as a PFS image's are.
static std::vector<std::byte> XtsEncrypt(std::vector<std::byte> bytes, u64 first) {
    std::array<unsigned char, 32> keys{};
    std::memcpy(keys.data(), DataKey.data(), 16);
    std::memcpy(keys.data() + 16, TweakKey.data(), 16);
    for (u64 sector = first; (sector + 1) * XtsSource::SectorSize <= bytes.size(); ++sector) {
        std::array<unsigned char, 16> tweak{};
        for (std::size_t i = 0; i < sizeof(sector); ++i) {
            tweak[i] = static_cast<unsigned char>(sector >> (8 * i));
        }
        EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
        int written = 0;
        auto* at = reinterpret_cast<unsigned char*>(bytes.data() + sector * XtsSource::SectorSize);
        EVP_EncryptInit_ex(context, EVP_aes_128_xts(), nullptr, keys.data(), tweak.data());
        EVP_EncryptUpdate(context, at, &written, at, XtsSource::SectorSize);
        EVP_CIPHER_CTX_free(context);
    }
    return bytes;
}

static void Xts() {
    const auto plain = Pattern(XtsSource::SectorSize * 3, 5);
    // Sectors 1 and 2 encrypted, as an image whose encryption starts at sector 1 has them.
    const XtsSource xts{std::make_shared<MemorySource>(XtsEncrypt(plain, 1)), DataKey, TweakKey, 1};
    Check(xts.ReadBytes(0, plain.size()) == plain,
          "XTS sectors decrypt, those before the first stay as they are");
}

static void Layers() {
    // As in a package: a PFSC image inside an encrypted one, a block of it many sectors long
    // (more than a thread caches), so making it caches many of the layer below's.
    constexpr u32 BlockSize = 0x10000;
    std::vector<std::byte> data(BlockSize);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i / 64);
    }
    const auto stored_block = Pattern(BlockSize, 9);
    data.insert(data.end(), stored_block.begin(), stored_block.end());
    auto image = MakePfsc(data, BlockSize);
    image.resize((image.size() + XtsSource::SectorSize - 1) / XtsSource::SectorSize *
                 XtsSource::SectorSize);
    const auto xts = std::make_shared<XtsSource>(
        std::make_shared<MemorySource>(XtsEncrypt(image, 0)), DataKey, TweakKey, 0);
    const PfscSource pfsc{xts};
    Check(pfsc.ReadBytes(0, data.size()) == data, "a PFSC image read through an encrypted one");
    Check(pfsc.ReadBytes(BlockSize + 7, 1) == std::vector<std::byte>{data[BlockSize + 7]},
          "a byte of a block that spans many sectors");
}

/// A small unsigned PFS image: blocks of 0x1000; the super root (inode 0) holds uroot (inode
/// 1), which holds a.bin (inode 2, "hello") and the folder sub (inode 3, empty).
static std::vector<std::byte> MakePfs() {
    constexpr std::size_t Block = 0x1000;
    constexpr std::size_t InodeSize = 0xa8;
    std::vector<std::byte> image(Block * 6);
    const auto put = [&](std::size_t at, u64 value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            image[at + i] = static_cast<std::byte>(value >> (8 * i));
        }
    };
    put(0, 1, 8);        // version
    put(8, 20130315, 8); // magic
    put(0x20, Block, 4);
    put(0x30, 4, 8); // inodes
    put(0x40, 1, 8); // blocks of inodes
    const auto inode = [&](std::size_t number, u64 size, u32 blocks, u32 first) {
        const std::size_t at = Block + number * InodeSize;
        put(at + 8, size, 8);
        put(at + 0x60, blocks, 4);
        put(at + 0x64, first, 4);
    };
    inode(0, Block, 1, 2);
    inode(1, Block, 1, 3);
    inode(2, 5, 1, 4);
    inode(3, Block, 1, 5);
    const auto dirent = [&](std::size_t at, u32 number, s32 type, std::string_view name) {
        const std::size_t size = (0x10 + name.size() + 8) & ~std::size_t{7};
        put(at, number, 4);
        put(at + 4, static_cast<u64>(type), 4);
        put(at + 8, name.size(), 4);
        put(at + 0xc, size, 4);
        std::memcpy(image.data() + at + 0x10, name.data(), name.size());
        return at + size;
    };
    dirent(2 * Block, 1, 3, "uroot");
    dirent(dirent(3 * Block, 2, 2, "a.bin"), 3, 3, "sub");
    std::memcpy(image.data() + 4 * Block, "hello", 5);
    return image;
}

static std::string Text(const Source& source) {
    const auto bytes = source.ReadBytes(0, static_cast<std::size_t>(source.Size()));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

static bool Lists(const Pfs& pfs, std::string_view folder, std::string_view name, bool directory) {
    return std::ranges::any_of(pfs.List(folder), [&](const Pfs::Entry& entry) {
        return entry.name == name && entry.directory == directory;
    });
}

static void PfsTree() {
    Pfs pfs{std::make_shared<MemorySource>(MakePfs())};
    Check(pfs.Files().size() == 1 && pfs.Find("a.bin") != nullptr, "a PFS image's files are found");
    Check(Text(*pfs.Open(*pfs.Find("a.bin"))) == "hello", "a file reads back");
    Check(pfs.IsDirectory("") && pfs.IsDirectory("sub") && !pfs.IsDirectory("a.bin"),
          "folders are told from files");
    Check(Lists(pfs, "", "a.bin", false) && Lists(pfs, "", "sub", true) && pfs.List("sub").empty(),
          "a folder lists its files and folders, an empty one nothing");
    Check(pfs.Find("missing") == nullptr && pfs.List("missing").empty(),
          "what is not there is not found");

    pfs.Add("sce_sys/trophy/trophy00.trp", std::make_shared<MemorySource>(Pattern(3, 0)));
    Check(pfs.IsDirectory("sce_sys") && pfs.IsDirectory("sce_sys/trophy") &&
              Lists(pfs, "", "sce_sys", true) && Lists(pfs, "sce_sys", "trophy", true) &&
              Lists(pfs, "sce_sys/trophy", "trophy00.trp", false),
          "an added file brings the folders on its way");
    Check(pfs.Open(*pfs.Find("sce_sys/trophy/trophy00.trp"))->ReadBytes(0, 3) == Pattern(3, 0),
          "an added file reads back");
    const std::string text = "replaced";
    pfs.Add("a.bin", std::make_shared<MemorySource>(std::vector<std::byte>(
                         reinterpret_cast<const std::byte*>(text.data()),
                         reinterpret_cast<const std::byte*>(text.data()) + text.size())));
    Check(Text(*pfs.Open(*pfs.Find("a.bin"))) == "replaced" && pfs.Files().size() == 2 &&
              std::ranges::count_if(pfs.List(""),
                                    [](const Pfs::Entry& e) { return e.name == "a.bin"; }) == 1,
          "adding a file that is there replaces it");
}

/// A source that counts the reads that reach it (a network share's, say).
class CountingSource final : public Source {
public:
    explicit CountingSource(std::vector<std::byte> bytes) : inner{std::move(bytes)} {}
    u64 Size() const override {
        return inner.Size();
    }
    void Read(u64 offset, std::span<std::byte> out) const override {
        ++reads;
        inner.Read(offset, out);
    }
    mutable std::atomic<int> reads{0};

private:
    MemorySource inner;
};

static void Cache() {
    constexpr u64 Chunk = CachedSource::ChunkSize;
    const auto bytes = Pattern(10 * Chunk + Chunk / 2, 11);
    const auto remote = std::make_shared<CountingSource>(bytes);
    const auto folder =
        std::filesystem::temp_directory_path() / ("pkg-cache-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto cache = folder / "game.cache";
    const auto expected = [&](u64 at, std::size_t count) {
        return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(at + count));
    };
    {
        const CachedSource cached{remote, cache, "game.pkg 1"};
        Check(cached.CachedBytes() == 0, "a new cache holds nothing");
        Check(cached.ReadBytes(100, 50) == expected(100, 50) && remote->reads == 1,
              "a miss is read from the remote");
        Check(cached.CachedBytes() == CachedSource::ReadAhead * Chunk,
              "a miss brings the chunks after it");
        Check(cached.ReadBytes(Chunk * 2 + 7, 1000) == expected(Chunk * 2 + 7, 1000) &&
                  remote->reads == 1,
              "what was brought is read locally");
        Check(cached.ReadBytes(bytes.size() - 10, 10) == expected(bytes.size() - 10, 10),
              "the short last chunk reads");
    }
    const int before = remote->reads;
    {
        const CachedSource again{remote, cache, "game.pkg 1"};
        Check(again.CachedBytes() >= CachedSource::ReadAhead * Chunk &&
                  again.ReadBytes(Chunk + 5, 64) == expected(Chunk + 5, 64) &&
                  remote->reads == before,
              "the cache is there for a later run");
        // Threads reading at once, misses among their reads.
        std::atomic<int> wrong{0};
        {
            std::vector<std::jthread> readers;
            for (int t = 0; t < 8; ++t) {
                readers.emplace_back([&, t] {
                    for (int i = 0; i < 200; ++i) {
                        const u64 at = (static_cast<u64>(i) * 104729 + static_cast<u64>(t) * 7919) %
                                       (bytes.size() - 5000);
                        if (again.ReadBytes(at, 5000) != expected(at, 5000)) {
                            ++wrong;
                        }
                    }
                });
            }
        }
        Check(wrong == 0, "threads reading through the cache at once read what is there");
        again.Fill({});
        Check(again.CachedBytes() == bytes.size() && again.ReadBytes(0, bytes.size()) == bytes,
              "filled, all of it is here");
    }
    {
        const CachedSource other{remote, cache, "game.pkg 2"};
        Check(other.CachedBytes() == 0, "a cache made for another file starts over");
        // A miss just now: a fill stopped sooner than the quiet it waits for fetches nothing.
        other.ReadBytes(0, 10);
        const u64 held = other.CachedBytes();
        std::stop_source stop;
        std::jthread fill{[&] { other.Fill(stop.get_token()); }};
        std::this_thread::sleep_for(CachedSource::FillAfterQuiet / 5);
        stop.request_stop();
        fill.join();
        Check(held < bytes.size() && other.CachedBytes() == held,
              "the fill waits while the title's reads need the remote");
        other.Fill({});
        Check(other.CachedBytes() == bytes.size(), "once they are quiet, it fills");
    }
    // Three caches of the same size, used in turn: the oldest goes first, the kept one never.
    const u64 one = bytes.size();
    for (const char* name : {"a", "b", "c"}) {
        const CachedSource made{remote, folder / name, name};
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    TrimCaches(folder, 3 * one + one, 0, std::array{folder / "a"}); // game.cache too: four, all fit
    Check(std::filesystem::exists(folder / "a") && std::filesystem::exists(folder / "game.cache"),
          "caches that fit are kept");
    TrimCaches(folder, 3 * one, one, std::array{folder / "a"});
    Check(!std::filesystem::exists(folder / "game.cache") &&
              !std::filesystem::exists(folder / "b") && std::filesystem::exists(folder / "a") &&
              std::filesystem::exists(folder / "c") && !std::filesystem::exists(folder / "b.map"),
          "the caches used longest ago make room, the kept one stays");
    std::filesystem::remove_all(folder);
}

static void BadMagic() {
    const auto zeros = std::make_shared<MemorySource>(std::vector<std::byte>(0x1000));
    Check(Throws([&] { Package{zeros}; }), "a package needs its magic");
    Check(Throws([&] { Pfs{zeros}; }), "a PFS image needs its magic");
}

int main() {
    EntryNames();
    Keys();
    Sources();
    Pfsc();
    Xts();
    Layers();
    PfsTree();
    Cache();
    BadMagic();
    std::cout << (failures == 0 ? "all passed" : std::to_string(failures) + " failed") << '\n';
    return failures == 0 ? 0 : 1;
}
