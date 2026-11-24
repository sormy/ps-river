// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "source.h"

namespace Pkg {

/// A PFSC image (the compressed form of a PFS image a package carries) read as the image.
class PfscSource final : public BlockSource {
public:
    static constexpr u32 Magic = 0x43534650; // "PFSC"
    static constexpr std::size_t HeaderSize = 0x30;

    explicit PfscSource(SourcePtr base);

private:
    void Load(u64 index, std::span<std::byte> block) const override;

    SourcePtr base;
    std::vector<u64> offsets; // where each block starts in `base`, and where the last ends
};

} // namespace Pkg
