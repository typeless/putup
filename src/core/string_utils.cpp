// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_utils.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include <array>
#include <cstdint>
#include <string_view>

namespace pup::core {

namespace {

constexpr auto stop_normal = std::uint8_t { 1 };
constexpr auto stop_double = std::uint8_t { 2 };
constexpr auto stop_single = std::uint8_t { 4 };

constexpr auto stop_at = [] {
    auto table = std::array<std::uint8_t, 256> {};
    table[static_cast<unsigned char>('\\')] = stop_normal | stop_double;
    table[static_cast<unsigned char>('"')] = stop_normal | stop_double;
    table[static_cast<unsigned char>('\'')] = stop_normal | stop_single;
    table[static_cast<unsigned char>(' ')] = stop_normal;
    table[static_cast<unsigned char>('\t')] = stop_normal;
    return table;
}();

} // namespace

auto tokenize_shell_command(std::string_view cmd) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto args = Vec<StringId> {};
    auto current = Buf {};
    auto in_single_quote = false;
    auto in_double_quote = false;
    auto escaped = false;

    for (auto i = std::size_t { 0 }; i < cmd.size();) {
        if (escaped) {
            current += cmd[i++];
            escaped = false;
            continue;
        }

        // One span per run: Buf::append(char) is two out-of-line calls for every byte.
        auto mask = in_single_quote ? stop_single : in_double_quote ? stop_double
                                                                    : stop_normal;
        auto stop = i;
        while (stop < cmd.size() && (stop_at[static_cast<unsigned char>(cmd[stop])] & mask) == 0) {
            ++stop;
        }
        if (stop > i) {
            current.append(cmd.substr(i, stop - i));
            i = stop;
            continue;
        }

        // The run scan stopped here, so c is special for this state's mask: the branches re-test nothing.
        auto c = cmd[i++];

        if (in_single_quote) {
            in_single_quote = false;
        } else if (in_double_quote) {
            if (c == '\\') {
                escaped = true;
            } else {
                in_double_quote = false;
            }
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '\'') {
            in_single_quote = true;
        } else if (c == '"') {
            in_double_quote = true;
        } else {
            // sh's blank class, deliberately: \r\v\f stay in words so the scan carries the compile's words; unquoted \n is sh's command separator, not a word break — #347.
            if (!current.empty()) {
                args.push_back(current.intern(pool));
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        args.push_back(current.intern(pool));
    }
    return args;
}

} // namespace pup::core
