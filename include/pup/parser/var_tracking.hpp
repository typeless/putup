// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "ast.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstdint>
#include <string_view>

namespace pup::parser {

/// Single variable assignment record
struct VarAssignment {
    StringId name = StringId::Empty;
    StringId filename = StringId::Empty;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    Assignment::Op op = Assignment::Op::Set;
    StringId value_before = StringId::Empty;
    StringId value_after = StringId::Empty;
    bool is_effective = true;
};

/// Collected assignment log
using AssignmentLog = Vec<VarAssignment>;

/// Grouped history for a single variable
struct VarHistory {
    StringId name = StringId::Empty;
    Vec<VarAssignment const*> assignments;
    StringId final_value = StringId::Empty;
};

/// Group assignments by variable name (returned sorted by name)
[[nodiscard]]
auto group_by_name(AssignmentLog const& log)
    -> Vec<VarHistory>;

/// Filter to assignments matching name (exact match)
[[nodiscard]]
auto filter_by_name(AssignmentLog const& log, std::string_view name)
    -> AssignmentLog;

/// Convert operator enum to string
[[nodiscard]]
auto op_to_string(Assignment::Op op) -> std::string_view;

} // namespace pup::parser
