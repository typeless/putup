// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

template<typename T>
concept FormatInt = std::integral<T> && !std::same_as<T, char> && !std::same_as<T, bool>;

struct Pad {
    long long value;
    int width;
    char fill;

    template<FormatInt T>
    Pad(T v, int width, char fill = ' ')
        : value(static_cast<long long>(v))
        , width(width)
        , fill(fill)
    {
    }
};

struct Fixed {
    double value;
    int precision;
    int width;

    Fixed(double v, int precision, int width = 0)
        : value(v)
        , precision(precision)
        , width(width)
    {
        assert(precision >= 0 && precision <= 6);
    }
};

struct FormatArg {
    enum class Tag : std::uint8_t { StringView,
                                    Long,
                                    Char,
                                    Fixed };

    Tag tag;
    int width = 0;
    char fill = ' ';
    std::uint8_t precision = 0;
    union {
        std::string_view sv;
        long long ll;
        char c;
        double d;
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
    template<FormatInt T>
    FormatArg(T v)
        : tag(Tag::Long)
        , ll(static_cast<long long>(v))
    {
    } // NOLINT
    FormatArg(char v)
        : tag(Tag::Char)
        , c(v)
    {
    } // NOLINT
    FormatArg(Pad p)
        : tag(Tag::Long)
        , width(p.width)
        , fill(p.fill)
        , ll(p.value)
    {
    } // NOLINT
    FormatArg(pup::Fixed f)
        : tag(Tag::Fixed)
        , width(f.width)
        , precision(static_cast<std::uint8_t>(f.precision))
        , d(f.value)
    {
    } // NOLINT
};

template<typename Buffer>
auto append_int(Buffer& out, long long v, int width, char fill) -> void
{
    char tmp[24];
    auto [end, ec] = std::to_chars(tmp, tmp + sizeof(tmp), v);
    auto s = std::string_view { tmp, static_cast<std::size_t>(end - tmp) };
    if (fill == '0' && v < 0) {
        out.append('-');
        s.remove_prefix(1);
        for (auto n = static_cast<int>(s.size()) + 1; n < width; ++n) {
            out.append('0');
        }
    } else {
        for (auto n = static_cast<int>(s.size()); n < width; ++n) {
            out.append(fill);
        }
    }
    out.append(s);
}

// Nearest with exact ties away from zero (not printf's ties-to-even); domain finite |v| < 9e15, larger saturates.
[[nodiscard]]
inline auto render_fixed(char (&buf)[32], double v, int precision) -> std::string_view
{
    auto* p = buf;
    if (v != v) {
        return "nan";
    }
    if (std::signbit(v)) {
        *p++ = '-';
        v = -v;
    }
    assert(v < 9.0e15);
    if (v > 9.0e15) {
        v = 9.0e15;
    }

    constexpr auto pow10 = std::array<long long, 7> { 1, 10, 100, 1000, 10000, 100000, 1000000 };
    auto const scale = pow10[precision];
    auto whole = static_cast<unsigned long long>(v);
    auto const frac_scaled = (v - static_cast<double>(whole)) * static_cast<double>(scale);
    auto fi = static_cast<unsigned long long>(frac_scaled);
    if (frac_scaled - static_cast<double>(fi) >= 0.5) {
        ++fi;
    }
    if (fi == static_cast<unsigned long long>(scale)) {
        fi = 0;
        ++whole;
    }

    p = std::to_chars(p, buf + sizeof(buf), whole).ptr;
    if (precision > 0) {
        *p++ = '.';
        for (auto i = precision - 1; i >= 0; --i) {
            p[i] = static_cast<char>('0' + fi % 10);
            fi /= 10;
        }
        p += precision;
    }
    return { buf, static_cast<std::size_t>(p - buf) };
}

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
                case FormatArg::Tag::Long:
                    append_int(out, arg.ll, arg.width, arg.fill);
                    break;
                case FormatArg::Tag::Char:
                    out.append(arg.c);
                    break;
                case FormatArg::Tag::Fixed: {
                    char tmp[32];
                    auto s = render_fixed(tmp, arg.d, arg.precision);
                    for (auto n = static_cast<int>(s.size()); n < arg.width; ++n) {
                        out.append(' ');
                    }
                    out.append(s);
                    break;
                }
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
