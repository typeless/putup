// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/robin_hood_index.hpp"

#include <cstdint>
#include <utility>

using pup::RobinHoodIndex;

namespace {

auto const always = [](std::uint32_t) { return true; };

} // namespace

TEST_CASE("RobinHoodIndex empty state", "[robin_hood_index]")
{
    auto idx = RobinHoodIndex {};

    REQUIRE(idx.count() == 0);
    REQUIRE_FALSE(idx.find(42, always).has_value());
}

TEST_CASE("RobinHoodIndex insert and find", "[robin_hood_index]")
{
    auto idx = RobinHoodIndex {};

    SECTION("finds an inserted value by hash")
    {
        idx.insert(7, 100);
        auto found = idx.find(7, always);
        REQUIRE(found.has_value());
        REQUIRE(*found == 100);
        REQUIRE(idx.count() == 1);
    }

    SECTION("missing hash returns nullopt")
    {
        idx.insert(7, 100);
        REQUIRE_FALSE(idx.find(8, always).has_value());
    }

    SECTION("reserved hash values 0 and 1 are usable")
    {
        idx.insert(0, 10);
        idx.insert(1, 11);
        REQUIRE(idx.find(0, [](std::uint32_t v) { return v == 10; }).has_value());
        REQUIRE(idx.find(1, [](std::uint32_t v) { return v == 11; }).has_value());
    }
}

TEST_CASE("RobinHoodIndex hash collisions resolve through the match predicate", "[robin_hood_index]")
{
    auto idx = RobinHoodIndex {};
    idx.insert(5, 100);
    idx.insert(5, 200);
    idx.insert(5, 300);

    auto find_exact = [&](std::uint32_t want) {
        return idx.find(5, [want](std::uint32_t v) { return v == want; });
    };

    REQUIRE(find_exact(100).has_value());
    REQUIRE(find_exact(200).has_value());
    REQUIRE(find_exact(300).has_value());
    REQUIRE_FALSE(find_exact(400).has_value());
    REQUIRE(idx.count() == 3);
}

TEST_CASE("RobinHoodIndex growth", "[robin_hood_index]")
{
    auto idx = RobinHoodIndex {};

    for (std::uint32_t i = 0; i < 10000; ++i) {
        idx.insert(i * 2654435761U, i);
    }
    REQUIRE(idx.count() == 10000);

    for (std::uint32_t i = 0; i < 10000; ++i) {
        auto found = idx.find(i * 2654435761U, [i](std::uint32_t v) { return v == i; });
        REQUIRE(found.has_value());
    }
}

TEST_CASE("RobinHoodIndex reserve", "[robin_hood_index]")
{
    auto idx = RobinHoodIndex {};
    idx.reserve(1000);

    for (std::uint32_t i = 0; i < 1000; ++i) {
        idx.insert(i, i);
    }
    REQUIRE(idx.count() == 1000);
    REQUIRE(idx.find(999, always).has_value());
}

TEST_CASE("RobinHoodIndex clear", "[robin_hood_index]")
{
    auto idx = RobinHoodIndex {};
    idx.insert(3, 30);
    idx.clear();
    REQUIRE(idx.count() == 0);
    REQUIRE_FALSE(idx.find(3, always).has_value());
}

TEST_CASE("RobinHoodIndex move semantics", "[robin_hood_index]")
{
    auto a = RobinHoodIndex {};
    a.insert(9, 90);

    auto b = RobinHoodIndex { std::move(a) };
    REQUIRE(b.count() == 1);
    REQUIRE(b.find(9, always).has_value());
    REQUIRE(a.count() == 0);

    auto c = RobinHoodIndex {};
    c = std::move(b);
    REQUIRE(c.count() == 1);
}
