// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/arena.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

auto build_slice(pup::Arena32& arena, std::uint32_t first, std::uint32_t count) -> pup::ArenaSlice
{
    auto s = pup::ArenaSlice {};
    for (std::uint32_t i = 0; i < count; ++i) {
        s = arena.append_extend(s, first + i);
    }
    return s;
}

} // namespace

TEST_CASE("Arena32 empty state", "[arena]")
{
    auto arena = pup::Arena32 {};

    SECTION("size is zero")
    {
        REQUIRE(arena.size() == 0);
    }

    SECTION("empty slice returns empty span")
    {
        auto span = arena.slice(pup::ArenaSlice { 0, 0 });
        REQUIRE(span.empty());
        REQUIRE(span.size() == 0);
    }
}

TEST_CASE("Arena32 append_extend grows a slice", "[arena]")
{
    auto arena = pup::Arena32 {};

    SECTION("extend from empty")
    {
        auto s = arena.append_extend(pup::ArenaSlice {}, 42);
        REQUIRE(s.length == 1);
        REQUIRE(arena.slice(s)[0] == 42);
    }

    SECTION("values survive block reallocations")
    {
        auto s = build_slice(arena, 0, 100);
        REQUIRE(s.length == 100);
        auto span = arena.slice(s);
        for (std::uint32_t i = 0; i < 100; ++i) {
            REQUIRE(span[i] == i);
        }
    }

    SECTION("span is iterable with range-for")
    {
        auto s = build_slice(arena, 5, 3);
        auto sum = std::uint32_t { 0 };
        for (auto v : arena.slice(s)) {
            sum += v;
        }
        REQUIRE(sum == 5 + 6 + 7);
    }
}

TEST_CASE("Arena32 interleaved extends stay linear in total size", "[arena]")
{
    auto arena = pup::Arena32 {};
    auto a = pup::ArenaSlice {};
    auto b = pup::ArenaSlice {};
    for (std::uint32_t i = 0; i < 1000; ++i) {
        a = arena.append_extend(a, i);
        b = arena.append_extend(b, 1000 + i);
    }
    REQUIRE(a.length == 1000);
    REQUIRE(b.length == 1000);
    auto sa = arena.slice(a);
    auto sb = arena.slice(b);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        REQUIRE(sa[i] == i);
        REQUIRE(sb[i] == 1000 + i);
    }
    REQUIRE(arena.size() <= std::size_t { 16000 });
}

TEST_CASE("Arena32 interleaved slices do not corrupt each other", "[arena]")
{
    auto arena = pup::Arena32 {};
    pup::ArenaSlice slices[8] = {};
    for (std::uint32_t round = 0; round < 37; ++round) {
        for (std::uint32_t k = 0; k < 8; ++k) {
            slices[k] = arena.append_extend(slices[k], k * 1000 + round);
        }
    }
    for (std::uint32_t k = 0; k < 8; ++k) {
        auto span = arena.slice(slices[k]);
        REQUIRE(span.size() == 37);
        for (std::uint32_t round = 0; round < 37; ++round) {
            REQUIRE(span[round] == k * 1000 + round);
        }
    }
}

TEST_CASE("Arena32 clear resets size", "[arena]")
{
    auto arena = pup::Arena32 {};
    (void)build_slice(arena, 0, 3);

    arena.clear();
    REQUIRE(arena.size() == 0);
}

TEST_CASE("Arena32 move semantics", "[arena]")
{
    auto a = pup::Arena32 {};
    auto const s = build_slice(a, 10, 2);

    SECTION("move constructor")
    {
        auto b = std::move(a);
        REQUIRE(b.slice(s)[0] == 10);
        REQUIRE(b.slice(s)[1] == 11);
    }

    SECTION("move assignment")
    {
        auto b = pup::Arena32 {};
        (void)build_slice(b, 99, 1);
        b = std::move(a);
        REQUIRE(b.slice(s)[0] == 10);
    }
}
