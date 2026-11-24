// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "source.h"

namespace Pkg {

/// Where a package entry goes in the game's sce_sys folder ("param.sfo", "trophy/trophy00.trp");
/// none for the package's own records and ids not known.
std::optional<std::string> EntryPath(u32 id);

/// EntryPath for the entries an installed game keeps: not the package's licence records, nor
/// the .dds copies of its pictures.
std::optional<std::string> InstalledEntryPath(u32 id);

} // namespace Pkg
