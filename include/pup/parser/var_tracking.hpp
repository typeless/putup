// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "ast.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pup::parser {

/// Single variable assignment record
struct VarAssignment {
    std::string name;
    std::string filename;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    Assignment::Op op = Assignment::Op::Set;
    std::string value_before;
    std::string value_after;
    bool is_effective = true;
};

/// Collected assignment log
using AssignmentLog = std::vector<VarAssignment>;

/// Grouped history for a single variable
struct VarHistory {
    std::string name;
    std::vector<VarAssignment const*> assignments;
    std::string final_value;
};

/// Group assignments by variable name
[[nodiscard]]
auto group_by_name(AssignmentLog const& log)
    -> std::map<std::string, VarHistory, std::less<>>;

/// Filter to assignments matching name (exact match)
[[nodiscard]]
auto filter_by_name(AssignmentLog const& log, std::string_view name)
    -> AssignmentLog;

/// Convert operator enum to string
[[nodiscard]]
auto op_to_string(Assignment::Op op) -> std::string_view;

} // namespace pup::parser
