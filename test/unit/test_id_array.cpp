// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/id_array.hpp"

#include <vector>

TEST_CASE("IdArray32 empty state", "[id_array]")
{
    auto arr = pup::IdArray32 {};

    SECTION("get returns 0 for any id")
    {
        REQUIRE(arr.get(0) == 0);
        REQUIRE(arr.get(42) == 0);
    }

    SECTION("contains returns false")
    {
        REQUIRE_FALSE(arr.contains(0));
        REQUIRE_FALSE(arr.contains(100));
    }

    SECTION("for_each visits nothing")
    {
        auto visited = std::size_t { 0 };
        arr.for_each(
            [](std::uint32_t, std::uint32_t, void* ctx) {
                ++*static_cast<std::size_t*>(ctx);
            },
            &visited
        );
        REQUIRE(visited == 0);
    }
}

TEST_CASE("IdArray32 set and get", "[id_array]")
{
    auto arr = pup::IdArray32 {};

    SECTION("single value")
    {
        arr.set(10, 42);
        REQUIRE(arr.get(10) == 42);
        REQUIRE(arr.contains(10));
    }

    SECTION("overwrite existing")
    {
        arr.set(5, 100);
        arr.set(5, 200);
        REQUIRE(arr.get(5) == 200);
        REQUIRE(arr.contains(5));
    }

    SECTION("unset returns 0")
    {
        arr.set(10, 42);
        REQUIRE(arr.get(11) == 0);
        REQUIRE_FALSE(arr.contains(11));
    }

    SECTION("set with value 0 is still present")
    {
        arr.set(7, 0);
        REQUIRE(arr.contains(7));
        REQUIRE(arr.get(7) == 0);
    }
}

TEST_CASE("IdArray32 clear", "[id_array]")
{
    auto arr = pup::IdArray32 {};
    arr.set(1, 10);
    arr.set(2, 20);
    arr.set(3, 30);

    arr.clear();
    REQUIRE_FALSE(arr.contains(1));
    REQUIRE_FALSE(arr.contains(2));
    REQUIRE_FALSE(arr.contains(3));
    REQUIRE(arr.get(1) == 0);
}

TEST_CASE("IdArray32 for_each iterates occupied slots", "[id_array]")
{
    auto arr = pup::IdArray32 {};
    arr.set(100, 1);
    arr.set(5, 2);
    arr.set(50, 3);

    auto collected = std::vector<std::pair<std::uint32_t, std::uint32_t>> {};
    arr.for_each(
        [](std::uint32_t id, std::uint32_t value, void* ctx) {
            static_cast<std::vector<std::pair<std::uint32_t, std::uint32_t>>*>(ctx)->emplace_back(id, value);
        },
        &collected
    );

    REQUIRE(collected.size() == 3);
    REQUIRE(collected[0] == std::pair<std::uint32_t, std::uint32_t> { 5, 2 });
    REQUIRE(collected[1] == std::pair<std::uint32_t, std::uint32_t> { 50, 3 });
    REQUIRE(collected[2] == std::pair<std::uint32_t, std::uint32_t> { 100, 1 });
}

TEST_CASE("IdArray32 move semantics", "[id_array]")
{
    auto a = pup::IdArray32 {};
    a.set(1, 10);
    a.set(2, 20);

    SECTION("move constructor")
    {
        auto b = std::move(a);
        REQUIRE(b.get(1) == 10);
        REQUIRE(b.get(2) == 20);
        REQUIRE(b.contains(1));
    }

    SECTION("move assignment")
    {
        auto b = pup::IdArray32 {};
        b.set(99, 99);
        b = std::move(a);
        REQUIRE(b.get(1) == 10);
        REQUIRE(b.contains(1));
        REQUIRE_FALSE(b.contains(99));
    }
}

