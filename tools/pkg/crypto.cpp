// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "crypto.h"

#include <algorithm>
#include <memory>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace Pkg {

namespace {

const unsigned char* In(std::span<const std::byte> data) {
    return reinterpret_cast<const unsigned char*>(data.data());
}

unsigned char* Out(std::span<std::byte> data) {
    return reinterpret_cast<unsigned char*>(data.data());
}

struct CipherFree {
    void operator()(EVP_CIPHER_CTX* context) const {
        EVP_CIPHER_CTX_free(context);
    }
};

/// Decrypts `data` in place with `cipher` (no padding).
void Decrypt(const EVP_CIPHER* cipher, std::span<const std::byte> key,
             std::span<const std::byte> iv, std::span<std::byte> data) {
    const std::unique_ptr<EVP_CIPHER_CTX, CipherFree> context{EVP_CIPHER_CTX_new()};
    int written = 0;
    int finished = 0;
    if (!context || EVP_DecryptInit_ex(context.get(), cipher, nullptr, In(key), In(iv)) != 1 ||
        EVP_CIPHER_CTX_set_padding(context.get(), 0) != 1 ||
        EVP_DecryptUpdate(context.get(), Out(data), &written, Out(data),
                          static_cast<int>(data.size())) != 1 ||
        EVP_DecryptFinal_ex(context.get(), Out(data) + written, &finished) != 1) {
        throw std::runtime_error("decryption failed");
    }
}

} // namespace

Digest Sha256(std::span<const std::byte> data) {
    Digest digest;
    SHA256(In(data), data.size(), Out(digest));
    return digest;
}

Digest HmacSha256(std::span<const std::byte> key, std::span<const std::byte> data) {
    Digest digest;
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), In(data), data.size(), Out(digest),
         &length);
    return digest;
}

void AesCbcDecrypt(const Key128& key, const Key128& iv, std::span<std::byte> data) {
    if (data.size() % 16 != 0) {
        throw FormatError("encrypted data not in whole AES blocks");
    }
    Decrypt(EVP_aes_128_cbc(), key, iv, data);
}

XtsSource::XtsSource(SourcePtr base, const Key128& data_key, const Key128& tweak_key,
                     u64 first_encrypted)
    : BlockSource{base->Size(), SectorSize}, base{std::move(base)},
      first_encrypted{first_encrypted} {
    std::ranges::copy(data_key, keys.begin());
    std::ranges::copy(tweak_key, keys.begin() + data_key.size());
}

void XtsSource::Load(u64 index, std::span<std::byte> block) const {
    const u64 offset = index * SectorSize;
    const std::size_t present = static_cast<std::size_t>(std::min<u64>(SectorSize, size - offset));
    std::ranges::fill(block, std::byte{0});
    base->Read(offset, block.first(present));
    if (index < first_encrypted) {
        return;
    }
    std::array<std::byte, 16> tweak{};
    for (std::size_t i = 0; i < sizeof(index); ++i) {
        tweak[i] = static_cast<std::byte>(index >> (8 * i));
    }
    Decrypt(EVP_aes_128_xts(), keys, tweak, block);
}

} // namespace Pkg
