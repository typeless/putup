// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pup::parser {

/// Parsed dependency file (.d file from gcc -MD)
struct Depfile {
    std::string target;                    ///< Output file (left side of colon)
    std::vector<std::string> dependencies; ///< Input files (right side of colon)
};

/// Parse a dependency file from a filesystem path
[[nodiscard]]
auto parse_depfile(std::filesystem::path const& path) -> Result<Depfile>;

/// Parse a dependency file from string content
[[nodiscard]]
auto parse_depfile(std::string_view content) -> Result<Depfile>;

} // namespace pup::parser
