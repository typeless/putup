// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/format_to.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using pup::Buf;
using pup::Fixed;
using pup::Pad;

namespace {

template<typename... Args>
auto fmt(std::string_view pattern, Args const&... args) -> std::string
{
    auto buf = Buf {};
    buf.fmt(pattern, args...);
    return std::string { buf.view() };
}

auto ref_pad(long long v, int width, char fill) -> std::string
{
    char out[64];
    auto n = fill == '0'
        ? std::snprintf(out, sizeof(out), "%0*lld", width, v)
        : std::snprintf(out, sizeof(out), "%*lld", width, v);
    return { out, static_cast<std::size_t>(n) };
}

auto ref_fixed(double v, int precision) -> std::string
{
    char out[64];
    auto n = std::snprintf(out, sizeof(out), "%.*f", precision, v);
    return { out, static_cast<std::size_t>(n) };
}

constexpr auto POW10_TABLE = std::array<long long, 7> { 1, 10, 100, 1000, 10000, 100000, 1000000 };

} // namespace

TEST_CASE("Pad right-aligns with spaces", "[format]")
{
    REQUIRE(fmt("{}", Pad { 42, 6 }) == "    42");
    REQUIRE(fmt("{}", Pad { 1234567, 3 }) == "1234567");
    REQUIRE(fmt("{}", Pad { -42, 6 }) == "   -42");
    REQUIRE(fmt("{}", Pad { std::size_t { 9 }, 6 }) == "     9");
    REQUIRE(fmt("{}", Pad { 0, 0 }) == "0");
}

TEST_CASE("Pad zero-fills after the sign", "[format]")
{
    REQUIRE(fmt("{}", Pad { 7, 2, '0' }) == "07");
    REQUIRE(fmt("{}", Pad { 59, 2, '0' }) == "59");
    REQUIRE(fmt("{}", Pad { -7, 4, '0' }) == "-007");
}

TEST_CASE("Fixed renders decimal digits", "[format]")
{
    REQUIRE(fmt("{}", Fixed { 3.0, 1 }) == "3.0");
    REQUIRE(fmt("{}", Fixed { 1.234, 1 }) == "1.2");
    REQUIRE(fmt("{}", Fixed { 9.96, 1 }) == "10.0");
    REQUIRE(fmt("{}", Fixed { 0.0, 1 }) == "0.0");
    REQUIRE(fmt("{}", Fixed { -0.0, 1 }) == "-0.0");
    REQUIRE(fmt("{}", Fixed { -1.25, 2 }) == "-1.25");
    REQUIRE(fmt("{}", Fixed { 7.4, 0 }) == "7");
}

TEST_CASE("Fixed pads to width with spaces", "[format]")
{
    REQUIRE(fmt("{}", Fixed { 1.2, 1, 6 }) == "   1.2");
    REQUIRE(fmt("{}", Fixed { 123.4, 1, 4 }) == "123.4");
    REQUIRE(fmt("{}", Fixed { -1.2, 1, 6 }) == "  -1.2");
}

TEST_CASE("Fixed rounds exact ties away from zero", "[format]")
{
    REQUIRE(fmt("{}", Fixed { 0.25, 1 }) == "0.3");
    REQUIRE(fmt("{}", Fixed { 0.75, 1 }) == "0.8");
    REQUIRE(fmt("{}", Fixed { -0.25, 1 }) == "-0.3");
    REQUIRE(fmt("{}", Fixed { 2.5, 0 }) == "3");
}

TEST_CASE("format accepts long and unsigned long long", "[format]")
{
    REQUIRE(fmt("{} {}", 5L, 7ULL) == "5 7");
    REQUIRE(fmt("{}", static_cast<unsigned long>(8)) == "8");
}

TEST_CASE("Pad matches snprintf across widths and values", "[format]")
{
    auto values = std::vector<long long> {};
    for (auto v = -1200LL; v <= 1200; ++v) {
        values.push_back(v);
    }
    for (auto p = 10LL; p <= 1000000000000000000LL; p *= 10) {
        values.push_back(p - 1);
        values.push_back(p);
        values.push_back(-p);
    }
    values.push_back(std::numeric_limits<long long>::max());
    values.push_back(std::numeric_limits<long long>::min());

    for (auto v : values) {
        for (auto w : { 0, 1, 2, 3, 6, 8, 20 }) {
            for (auto f : { ' ', '0' }) {
                CAPTURE(v, w, f);
                REQUIRE(fmt("{}", Pad { v, w, f }) == ref_pad(v, w, f));
            }
        }
    }
}

TEST_CASE("Fixed parse-back stays within half a last-digit ulp", "[format]")
{
    for (auto p = 0; p <= 6; ++p) {
        auto const scale = static_cast<double>(POW10_TABLE[p]);
        for (auto k = -5000; k <= 5000; k += 7) {
            auto const v = k / 1000.0;
            auto const s = fmt("{}", Fixed { v, p });
            auto const parsed = std::strtod(s.c_str(), nullptr);
            CAPTURE(v, p, s);
            REQUIRE(std::fabs(parsed - v) <= 0.5 / scale + 1e-12);
        }
    }
}

TEST_CASE("Fixed matches snprintf on dyadic values off exact ties", "[format]")
{
    for (auto p = 0; p <= 3; ++p) {
        auto const scale = static_cast<double>(POW10_TABLE[p]);
        for (auto k = -4096; k <= 4096; ++k) {
            auto const v = k / 16.0;
            auto const a = std::fabs(v);
            auto const frac_scaled = (a - std::floor(a)) * scale;
            auto const is_exact_tie = frac_scaled - std::floor(frac_scaled) == 0.5;
            if (is_exact_tie) {
                continue;
            }
            CAPTURE(v, p);
            REQUIRE(fmt("{}", Fixed { v, p }) == ref_fixed(v, p));
        }
    }
}
