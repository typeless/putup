// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_utils.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include <algorithm>
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
    table[static_cast<unsigned char>('`')] = stop_normal | stop_double;
    table[static_cast<unsigned char>('$')] = stop_normal | stop_double;
    table[static_cast<unsigned char>('\'')] = stop_normal | stop_single;
    table[static_cast<unsigned char>(' ')] = stop_normal;
    table[static_cast<unsigned char>('\t')] = stop_normal;
    table[static_cast<unsigned char>(';')] = stop_normal;
    table[static_cast<unsigned char>('&')] = stop_normal;
    table[static_cast<unsigned char>('|')] = stop_normal;
    table[static_cast<unsigned char>('<')] = stop_normal;
    table[static_cast<unsigned char>('>')] = stop_normal;
    table[static_cast<unsigned char>('\n')] = stop_normal;
    return table;
}();

auto opens_substitution(std::string_view cmd, std::size_t i) -> bool
{
    return cmd[i] == '`' || (cmd[i] == '$' && i + 1 < cmd.size() && cmd[i + 1] == '(');
}

auto substitution_end(std::string_view cmd, std::size_t start) -> std::size_t
{
    if (cmd[start] == '`') {
        for (auto i = start + 1; i < cmd.size(); ++i) {
            if (cmd[i] == '\\') {
                ++i;
            } else if (cmd[i] == '`') {
                return i + 1;
            }
        }
        return cmd.size();
    }

    auto depth = 0;
    auto in_double_quote = false;
    for (auto i = start + 1; i < cmd.size();) {
        if (cmd[i] == '\\') {
            i += 2;
            continue;
        }
        if (opens_substitution(cmd, i)) {
            i = substitution_end(cmd, i);
            continue;
        }
        auto const c = cmd[i++];
        if (c == '"') {
            in_double_quote = !in_double_quote;
        } else if (in_double_quote) {
            continue;
        } else if (c == '\'') {
            auto const close = cmd.find('\'', i);
            i = close == std::string_view::npos ? cmd.size() : close + 1;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')' && --depth == 0) {
            return i;
        }
    }
    return cmd.size();
}

} // namespace

auto tokenize_shell_command(std::string_view cmd) -> Vec<ShellToken>
{
    auto& pool = global_pool();
    auto args = Vec<ShellToken> {};
    auto current = Buf {};
    auto in_single_quote = false;
    auto in_double_quote = false;
    auto escaped = false;
    auto word_quoted = false;
    auto redirection_end = std::size_t { 0 };
    auto flush_word = [&] {
        if (!current.empty() || word_quoted) {
            args.push_back(ShellToken { .text = current.intern(pool), .kind = ShellTokenKind::Word });
            current.clear();
        }
        word_quoted = false;
        redirection_end = 0;
    };

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

        if (opens_substitution(cmd, i)) {
            auto const end = substitution_end(cmd, i);
            current.append(cmd.substr(i, end - i));
            i = end;
            continue;
        }

        // The run scan stopped here, so c is special for this state's mask: the branches re-test nothing.
        auto c = cmd[i++];

        if (c == '\\' && i < cmd.size() && cmd[i] == '\n') {
            ++i;
            continue;
        }

        if (in_single_quote) {
            in_single_quote = false;
        } else if (in_double_quote) {
            if (c == '\\' && i < cmd.size() && std::string_view { "$`\"\\" }.find(cmd[i]) != std::string_view::npos) {
                escaped = true;
            } else if (c == '\\') {
                current += c;
            } else if (c == '"') {
                in_double_quote = false;
            } else {
                current += c;
            }
        } else if (c == '$') {
            current += c;
        } else if (c == '\\' && i == cmd.size()) {
            current += c;
        } else if (c == '\\') {
            escaped = true;
            word_quoted = true;
        } else if (c == '\'') {
            in_single_quote = true;
            word_quoted = true;
        } else if (c == '"') {
            in_double_quote = true;
            word_quoted = true;
        } else if (c == '<' || c == '>') {
            auto const ends_in_redirection = redirection_end != 0 && redirection_end == current.size();
            auto const is_descriptor = !word_quoted
                && std::ranges::all_of(current.view(), [](char d) { return d >= '0' && d <= '9'; });
            if (!current.empty() && !ends_in_redirection && !is_descriptor) {
                flush_word();
            }
            current += c;
            redirection_end = current.size();
        } else if ((c == '&' || c == '|') && redirection_end != 0 && redirection_end == current.size()) {
            current += c;
        } else if (c == ';' || c == '&' || c == '|' || c == '\n') {
            flush_word();
            auto const doubled = (c == '&' || c == '|') && i < cmd.size() && cmd[i] == c;
            args.push_back(ShellToken { .text = pool.intern(cmd.substr(i - 1, doubled ? 2 : 1)), .kind = ShellTokenKind::Operator });
            i += doubled ? 1 : 0;
        } else {
            // sh's blank class, deliberately: \r\v\f stay in words so the scan carries the compile's words.
            flush_word();
        }
    }
    flush_word();
    return args;
}

} // namespace pup::core
