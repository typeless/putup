// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/var_tracking.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

#include <algorithm>

namespace pup::parser {

auto group_by_name(AssignmentLog const& log)
    -> Vec<VarHistory>
{
    auto result = Vec<VarHistory> {};

    for (auto const& assign : log) {
        auto it = std::lower_bound(result.begin(), result.end(), assign.name, [](auto const& h, auto const& n) { return h.name < n; });
        if (it == result.end() || it->name != assign.name) {
            it = result.insert(it, VarHistory { .name = assign.name, .assignments = {}, .final_value = {} });
        }
        it->assignments.push_back(&assign);
        if (assign.is_effective) {
            it->final_value = assign.value_after;
        }
    }

    return result;
}

auto filter_by_name(AssignmentLog const& log, std::string_view name)
    -> AssignmentLog
{
    auto result = AssignmentLog {};
    for (auto const& assign : log) {
        if (global_pool().get(assign.name) == name) {
            result.push_back(assign);
        }
    }
    return result;
}

auto op_to_string(Assignment::Op op) -> std::string_view
{
    switch (op) {
    case Assignment::Op::Set:
        return "=";
    case Assignment::Op::Append:
        return "+=";
    case Assignment::Op::Define:
        return ":=";
    case Assignment::Op::SoftSet:
        return "?=";
    case Assignment::Op::WeakSet:
        return "?\?=";
    }
    return "=";
}

} // namespace pup::parser
