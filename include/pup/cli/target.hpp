// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string.hpp"
#include "pup/core/vec.hpp"

#include <optional>

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
) -> Vec<Target>;

[[nodiscard]]
auto validate_target_consistency(
    std::string_view project_root,
    Vec<String> const& targets
) -> Result<Vec<Target>>;

} // namespace pup
