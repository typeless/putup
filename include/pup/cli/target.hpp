// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pup {

struct Target {
    std::optional<String> variant;
    String scope_or_output;
    bool is_output = false;
};

[[nodiscard]]
auto parse_target(
    std::string_view project_root,
    std::string_view target_path
) -> Result<Target>;

[[nodiscard]]
auto expand_glob_target(
    std::string_view project_root,
    std::string_view pattern
) -> std::vector<Target>;

[[nodiscard]]
auto validate_target_consistency(
    std::string_view project_root,
    std::vector<String> const& targets
) -> Result<std::vector<Target>>;

} // namespace pup
