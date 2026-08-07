// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_utils.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include <string_view>

namespace pup::core {

auto tokenize_shell_command(std::string_view cmd) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto args = Vec<StringId> {};
    auto current = Buf {};
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
                // Narrower than is_space by design: this break class feeds command identity (#343).
                if (!current.empty()) {
                    args.push_back(current.intern(pool));
                    current.clear();
                }
            } else {
                current += c;
            }
        }
    }
    if (!current.empty()) {
        args.push_back(current.intern(pool));
    }
    return args;
}

} // namespace pup::core
