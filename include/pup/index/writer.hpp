// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "entry.hpp"
#include "format.hpp"
#include "pup/core/result.hpp"

#include <filesystem>
#include <vector>

namespace pup::index {

/// Write an index to a file atomically
/// Uses a temporary file and rename for atomic operation
[[nodiscard]]
auto write_index(
    std::filesystem::path const& path,
    Index const& index
) -> Result<void>;

/// Serialize an index to a byte vector
[[nodiscard]]
auto serialize_index(Index const& index) -> Result<std::vector<std::byte>>;

} // namespace pup::index
