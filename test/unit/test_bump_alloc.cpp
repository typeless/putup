// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/bump_alloc.hpp"
#include "pup/core/vec.hpp"

#include <array>
#include <cstddef>
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

TEST_CASE("relocate copies the bytes and adds them to the copied count", "[bump]")
{
    auto const source = std::array<std::uint8_t, 5> { 1, 2, 3, 4, 5 };
    auto target = std::array<std::uint8_t, 5> {};
    auto const before = pup::copied_bytes();

    pup::relocate(target.data(), source.data(), source.size());

    REQUIRE(target == source);
    REQUIRE(pup::copied_bytes() - before == source.size());
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
