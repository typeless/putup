// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/var_tracking.hpp"

namespace pup::parser {

auto group_by_name(AssignmentLog const& log)
    -> std::map<std::string, VarHistory, std::less<>>
{
    auto result = std::map<std::string, VarHistory, std::less<>> {};

    for (auto const& assign : log) {
        auto& history = result[assign.name];
        if (history.name.empty()) {
            history.name = assign.name;
        }
        history.assignments.push_back(&assign);
        if (assign.is_effective) {
            history.final_value = assign.value_after;
        }
    }

    return result;
}

auto filter_by_name(AssignmentLog const& log, std::string_view name)
    -> AssignmentLog
{
    auto result = AssignmentLog {};
    for (auto const& assign : log) {
        if (assign.name == name) {
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
