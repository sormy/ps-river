// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "source.h"

namespace Pkg {

using Digest = std::array<std::byte, 32>;
using Key128 = std::array<std::byte, 16>;

Digest Sha256(std::span<const std::byte> data);
Digest HmacSha256(std::span<const std::byte> key, std::span<const std::byte> data);
/// AES-128-CBC without padding, in place (`data` a whole number of 16-byte blocks).
void AesCbcDecrypt(const Key128& key, const Key128& iv, std::span<std::byte> data);

/// A PFS image's sectors decrypted with AES-128-XTS (the tweak a sector's index); sectors before
/// `first_encrypted` are as they are.
class XtsSource final : public BlockSource {
public:
    static constexpr u32 SectorSize = 0x1000;

    XtsSource(SourcePtr base, const Key128& data_key, const Key128& tweak_key, u64 first_encrypted);

private:
    void Load(u64 index, std::span<std::byte> block) const override;

    SourcePtr base;
    std::array<std::byte, 32> keys; // data key, then tweak key: XTS's order
    u64 first_encrypted;
};

} // namespace Pkg
