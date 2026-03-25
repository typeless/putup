// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/fmt.hpp"

#include <charconv>

namespace pup {

namespace {

auto append_int(String& out, int value) -> void
{
    char buf[16];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(std::string_view { buf, static_cast<std::size_t>(ptr - buf) });
}

} // namespace

auto format_impl(std::string_view pattern, FormatArg const* args, std::size_t count) -> String
{
    auto result = String {};
    result.reserve(pattern.size());

    auto arg_idx = std::size_t { 0 };
    auto pos = std::size_t { 0 };

    while (pos < pattern.size()) {
        // Look for next '{' or '}}'
        auto open = pattern.find('{', pos);
        auto close_close = pattern.find("}}", pos);

        // Handle }} escape before the next {
        if (close_close != std::string_view::npos && (open == std::string_view::npos || close_close < open)) {
            result.append(pattern.substr(pos, close_close - pos));
            result.append('}');
            pos = close_close + 2;
            continue;
        }

        auto brace = open;
        if (brace == std::string_view::npos) {
            result.append(pattern.substr(pos));
            break;
        }

        // Append literal text before the brace
        result.append(pattern.substr(pos, brace - pos));

        if (brace + 1 < pattern.size() && pattern[brace + 1] == '{') {
            // Escaped {{ → literal {
            result.append('{');
            pos = brace + 2;
        } else if (brace + 1 < pattern.size() && pattern[brace + 1] == '}') {
            // {} placeholder — substitute next argument
            if (arg_idx < count) {
                auto const& arg = args[arg_idx++];
                switch (arg.tag) {
                case FormatArg::Tag::StringView:
                    result.append(arg.sv);
                    break;
                case FormatArg::Tag::Int:
                    append_int(result, arg.i);
                    break;
                case FormatArg::Tag::Char:
                    result.append(arg.c);
                    break;
                }
            }
            pos = brace + 2;
        } else {
            // Lone { — pass through
            result.append('{');
            pos = brace + 1;
        }
    }

    return result;
}

} // namespace pup
