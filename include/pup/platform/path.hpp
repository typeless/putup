// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <filesystem>
#include <string>

namespace pup::platform {

/// Convert a filesystem path to a UTF-8 encoded string.
/// On POSIX, paths are already UTF-8. On Windows, this converts from UTF-16.
[[nodiscard]]
auto to_utf8(std::filesystem::path const& path) -> std::string;

} // namespace pup::platform
