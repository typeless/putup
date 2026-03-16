// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/id_bitset.hpp"

#include <vector>

TEST_CASE("IdBitSet empty state", "[id_bitset]")
{
    auto bs = pup::IdBitSet {};

    SECTION("count is zero")
    {
        REQUIRE(bs.count() == 0);
    }

    SECTION("contains returns false for any id")
    {
        REQUIRE_FALSE(bs.contains(0));
        REQUIRE_FALSE(bs.contains(42));
        REQUIRE_FALSE(bs.contains(1000));
    }

    SECTION("remove on empty is safe")
    {
        bs.remove(0);
        bs.remove(999);
        REQUIRE(bs.count() == 0);
    }

    SECTION("for_each visits nothing")
    {
        auto visited = std::size_t { 0 };
        bs.for_each(
            [](std::uint32_t, void* ctx) {
                ++*static_cast<std::size_t*>(ctx);
            },
            &visited
        );
        REQUIRE(visited == 0);
    }
}

TEST_CASE("IdBitSet insert and contains", "[id_bitset]")
{
    auto bs = pup::IdBitSet {};

    SECTION("single insert")
    {
        bs.insert(10);
        REQUIRE(bs.contains(10));
        REQUIRE_FALSE(bs.contains(11));
        REQUIRE(bs.count() == 1);
    }

    SECTION("id 0")
    {
        bs.insert(0);
        REQUIRE(bs.contains(0));
        REQUIRE(bs.count() == 1);
    }

    SECTION("duplicate insert is idempotent")
    {
        bs.insert(5);
        bs.insert(5);
        bs.insert(5);
        REQUIRE(bs.count() == 1);
        REQUIRE(bs.contains(5));
    }

    SECTION("multiple ids")
    {
        bs.insert(1);
        bs.insert(100);
        bs.insert(200);
        REQUIRE(bs.count() == 3);
        REQUIRE(bs.contains(1));
        REQUIRE(bs.contains(100));
        REQUIRE(bs.contains(200));
        REQUIRE_FALSE(bs.contains(2));
    }
}

TEST_CASE("IdBitSet word boundary", "[id_bitset]")
{
    auto bs = pup::IdBitSet {};

    SECTION("id 63 and 64 span word boundary")
    {
        bs.insert(63);
        bs.insert(64);
        REQUIRE(bs.contains(63));
        REQUIRE(bs.contains(64));
        REQUIRE_FALSE(bs.contains(62));
        REQUIRE_FALSE(bs.contains(65));
        REQUIRE(bs.count() == 2);
    }

    SECTION("all bits in first word")
    {
        for (auto i = std::uint32_t { 0 }; i < 64; ++i) {
            bs.insert(i);
        }
        REQUIRE(bs.count() == 64);
        for (auto i = std::uint32_t { 0 }; i < 64; ++i) {
            REQUIRE(bs.contains(i));
        }
        REQUIRE_FALSE(bs.contains(64));
    }
}

TEST_CASE("IdBitSet remove", "[id_bitset]")
{
    auto bs = pup::IdBitSet {};
    bs.insert(10);
    bs.insert(20);
    bs.insert(30);

    SECTION("remove existing id")
    {
        bs.remove(20);
        REQUIRE_FALSE(bs.contains(20));
        REQUIRE(bs.contains(10));
        REQUIRE(bs.contains(30));
        REQUIRE(bs.count() == 2);
    }

    SECTION("remove non-existing id is safe")
    {
        bs.remove(99);
        REQUIRE(bs.count() == 3);
    }
}

TEST_CASE("IdBitSet clear", "[id_bitset]")
{
    auto bs = pup::IdBitSet {};
    bs.insert(1);
    bs.insert(2);
    bs.insert(3);

    bs.clear();
    REQUIRE(bs.count() == 0);
    REQUIRE_FALSE(bs.contains(1));
    REQUIRE_FALSE(bs.contains(2));
    REQUIRE_FALSE(bs.contains(3));
}

TEST_CASE("IdBitSet for_each iteration order", "[id_bitset]")
{
    auto bs = pup::IdBitSet {};
    bs.insert(200);
    bs.insert(5);
    bs.insert(63);
    bs.insert(64);
    bs.insert(0);

    auto collected = std::vector<std::uint32_t> {};
    bs.for_each(
        [](std::uint32_t id, void* ctx) {
            static_cast<std::vector<std::uint32_t>*>(ctx)->push_back(id);
        },
        &collected
    );

    REQUIRE(collected == std::vector<std::uint32_t> { 0, 5, 63, 64, 200 });
}

TEST_CASE("IdBitSet move semantics", "[id_bitset]")
{
    auto a = pup::IdBitSet {};
    a.insert(42);
    a.insert(100);

    SECTION("move constructor")
    {
        auto b = std::move(a);
        REQUIRE(b.contains(42));
        REQUIRE(b.contains(100));
        REQUIRE(b.count() == 2);
    }

    SECTION("move assignment")
    {
        auto b = pup::IdBitSet {};
        b.insert(7);
        b = std::move(a);
        REQUIRE(b.contains(42));
        REQUIRE(b.contains(100));
        REQUIRE_FALSE(b.contains(7));
    }
}
