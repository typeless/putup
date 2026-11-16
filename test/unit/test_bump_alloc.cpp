// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/bump_alloc.hpp"
#include "pup/core/vec.hpp"

#include <cstdint>

TEST_CASE("growing a Vec that is not on top of the heap counts its old bytes as copied", "[bump]")
{
    auto v = pup::Vec<std::uint32_t> {};
    v.reserve(4);
    for (auto i = std::uint32_t { 0 }; i < 4; ++i) {
        v.push_back(i);
    }
    pup::bump_alloc(1, 1);
    auto const before = pup::copied_bytes();

    v.reserve(64);

    REQUIRE(pup::copied_bytes() - before == 4 * sizeof(std::uint32_t));
}

TEST_CASE("growing a Vec on top of the heap copies nothing", "[bump]")
{
    auto v = pup::Vec<std::uint32_t> {};
    v.reserve(4);
    v.push_back(1);
    auto const before = pup::copied_bytes();

    v.reserve(64);

    REQUIRE(pup::copied_bytes() == before);
}
