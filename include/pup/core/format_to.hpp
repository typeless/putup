// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

struct FormatArg {
    enum class Tag : std::uint8_t { StringView,
                                    Long,
                                    Char };

    Tag tag;
    union {
        std::string_view sv;
        long long ll;
        char c;
    };

    FormatArg(std::string_view s)
        : tag(Tag::StringView)
        , sv(s)
    {
    } // NOLINT
    FormatArg(char const* s)
        : tag(Tag::StringView)
        , sv(s)
    {
    } // NOLINT
    FormatArg(int v)
        : tag(Tag::Long)
        , ll(v)
    {
    } // NOLINT
    FormatArg(long long v)
        : tag(Tag::Long)
        , ll(v)
    {
    } // NOLINT
    FormatArg(unsigned int v)
        : tag(Tag::Long)
        , ll(v)
    {
    } // NOLINT
    FormatArg(std::size_t v)
        : tag(Tag::Long)
        , ll(static_cast<long long>(v))
    {
    } // NOLINT
    FormatArg(char v)
        : tag(Tag::Char)
        , c(v)
    {
    } // NOLINT
};

template<typename Buffer>
auto format_to(Buffer& out, std::string_view pattern, FormatArg const* args, std::size_t count) -> void
{
    auto arg_idx = std::size_t { 0 };
    auto pos = std::size_t { 0 };

    while (pos < pattern.size()) {
        auto open = pattern.find('{', pos);
        auto close_close = pattern.find("}}", pos);

        if (close_close != std::string_view::npos && (open == std::string_view::npos || close_close < open)) {
            out.append(pattern.substr(pos, close_close - pos));
            out.append('}');
            pos = close_close + 2;
            continue;
        }

        auto brace = open;
        if (brace == std::string_view::npos) {
            out.append(pattern.substr(pos));
            break;
        }

        out.append(pattern.substr(pos, brace - pos));

        if (brace + 1 < pattern.size() && pattern[brace + 1] == '{') {
            out.append('{');
            pos = brace + 2;
        } else if (brace + 1 < pattern.size() && pattern[brace + 1] == '}') {
            if (arg_idx < count) {
                auto const& arg = args[arg_idx++];
                switch (arg.tag) {
                case FormatArg::Tag::StringView:
                    out.append(arg.sv);
                    break;
                case FormatArg::Tag::Long: {
                    char tmp[24];
                    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), arg.ll);
                    out.append(std::string_view { tmp, static_cast<std::size_t>(ptr - tmp) });
                    break;
                }
                case FormatArg::Tag::Char:
                    out.append(arg.c);
                    break;
                }
            }
            pos = brace + 2;
        } else {
            out.append('{');
            pos = brace + 1;
        }
    }

    assert(arg_idx == count && "pup::fmt: argument count mismatch");
}

} // namespace pup
