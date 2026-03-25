// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_utils.hpp"

namespace pup::core {

auto tokenize_shell_command(std::string_view cmd) -> Vec<String>
{
    auto args = Vec<String> {};
    auto current = String {};
    auto in_single_quote = false;
    auto in_double_quote = false;
    auto escaped = false;

    for (auto c : cmd) {
        if (escaped) {
            current += c;
            escaped = false;
            continue;
        }

        if (in_single_quote) {
            if (c == '\'') {
                in_single_quote = false;
            } else {
                current += c;
            }
        } else if (in_double_quote) {
            if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_double_quote = false;
            } else {
                current += c;
            }
        } else {
            if (c == '\\') {
                escaped = true;
            } else if (c == '\'') {
                in_single_quote = true;
            } else if (c == '"') {
                in_double_quote = true;
            } else if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    args.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current += c;
            }
        }
    }
    if (!current.empty()) {
        args.push_back(std::move(current));
    }
    return args;
}

} // namespace pup::core
