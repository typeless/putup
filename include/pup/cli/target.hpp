// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pup {

struct Target {
    std::optional<std::string> variant;
    std::string scope_or_output;
    bool is_output = false;
};

[[nodiscard]]
auto parse_target(
    std::string const& project_root,
    std::string const& target_path
) -> Result<Target>;

[[nodiscard]]
auto expand_glob_target(
    std::string const& project_root,
    std::string const& pattern
) -> std::vector<Target>;

[[nodiscard]]
auto validate_target_consistency(
    std::string const& project_root,
    std::vector<std::string> const& targets
) -> Result<std::vector<Target>>;

} // namespace pup
