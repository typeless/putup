// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/arena.hpp"

#include <cstdint>

TEST_CASE("Arena32 empty state", "[arena]")
{
    auto arena = pup::Arena32 {};

    SECTION("size is zero")
    {
        REQUIRE(arena.size() == 0);
    }

    SECTION("empty slice get returns nullptr")
    {
        auto const slice = pup::ArenaSlice { 0, 0 };
        REQUIRE(arena.get(slice) == nullptr);
    }
}

TEST_CASE("Arena32 append and get", "[arena]")
{
    auto arena = pup::Arena32 {};

    SECTION("single append")
    {
        std::uint32_t const vals[] = { 10, 20, 30 };
        auto const slice = arena.append(vals, 3);

        REQUIRE(slice.offset == 0);
        REQUIRE(slice.length == 3);
        REQUIRE(arena.size() == 3);

        auto const* p = arena.get(slice);
        REQUIRE(p != nullptr);
        REQUIRE(p[0] == 10);
        REQUIRE(p[1] == 20);
        REQUIRE(p[2] == 30);
    }

    SECTION("empty append")
    {
        auto const slice = arena.append(nullptr, 0);
        REQUIRE(slice.length == 0);
        REQUIRE(arena.size() == 0);
    }
}

TEST_CASE("Arena32 multiple appends are contiguous", "[arena]")
{
    auto arena = pup::Arena32 {};

    std::uint32_t const a_vals[] = { 1, 2 };
    auto const a = arena.append(a_vals, 2);

    std::uint32_t const b_vals[] = { 3, 4, 5 };
    auto const b = arena.append(b_vals, 3);

    REQUIRE(a.offset == 0);
    REQUIRE(a.length == 2);
    REQUIRE(b.offset == 2);
    REQUIRE(b.length == 3);
    REQUIRE(arena.size() == 5);

    auto const* pa = arena.get(a);
    auto const* pb = arena.get(b);
    REQUIRE(pa[0] == 1);
    REQUIRE(pa[1] == 2);
    REQUIRE(pb[0] == 3);
    REQUIRE(pb[1] == 4);
    REQUIRE(pb[2] == 5);
}

TEST_CASE("Arena32 compact shrinks allocation", "[arena]")
{
    auto arena = pup::Arena32 {};
    arena.reserve(1000);

    std::uint32_t const vals[] = { 42 };
    arena.append(vals, 1);

    REQUIRE(arena.size() == 1);

    arena.compact();
    REQUIRE(arena.size() == 1);

    auto const slice = pup::ArenaSlice { 0, 1 };
    REQUIRE(arena.get(slice)[0] == 42);
}

TEST_CASE("Arena32 reserve pre-allocates", "[arena]")
{
    auto arena = pup::Arena32 {};
    arena.reserve(100);

    std::uint32_t const vals[] = { 7, 8, 9 };
    auto const slice = arena.append(vals, 3);
    REQUIRE(arena.get(slice)[0] == 7);
    REQUIRE(arena.size() == 3);
}

TEST_CASE("Arena32 clear resets size", "[arena]")
{
    auto arena = pup::Arena32 {};
    std::uint32_t const vals[] = { 1, 2, 3 };
    arena.append(vals, 3);

    arena.clear();
    REQUIRE(arena.size() == 0);
}

TEST_CASE("Arena32 move semantics", "[arena]")
{
    auto a = pup::Arena32 {};
    std::uint32_t const vals[] = { 10, 20 };
    auto const slice = a.append(vals, 2);

    SECTION("move constructor")
    {
        auto b = std::move(a);
        REQUIRE(b.size() == 2);
        REQUIRE(b.get(slice)[0] == 10);
        REQUIRE(b.get(slice)[1] == 20);
    }

    SECTION("move assignment")
    {
        auto b = pup::Arena32 {};
        std::uint32_t const other_vals[] = { 99 };
        b.append(other_vals, 1);
        b = std::move(a);
        REQUIRE(b.size() == 2);
        REQUIRE(b.get(slice)[0] == 10);
    }
}
