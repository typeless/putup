// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/region.hpp"
#include "pup/platform/vm.hpp"

#include <cstring>
#include <utility>

using pup::Region;

TEST_CASE("Region empty state", "[region]")
{
    auto r = Region {};

    SECTION("holds no memory until first ensure")
    {
        REQUIRE(r.data() == nullptr);
        REQUIRE(r.committed() == 0);
    }
}

TEST_CASE("Region ensure", "[region]")
{
    auto const page = pup::platform::vm::page_size();
    auto r = Region {};

    SECTION("commits at least the requested bytes, page-rounded")
    {
        r.ensure(100);
        REQUIRE(r.data() != nullptr);
        REQUIRE(r.committed() >= 100);
        REQUIRE(r.committed() % page == 0);
    }

    SECTION("fresh memory reads as zero")
    {
        r.ensure(2 * page);
        auto const* bytes = static_cast<unsigned char const*>(r.data());
        for (std::size_t i = 0; i < 2 * page; ++i) {
            REQUIRE(bytes[i] == 0);
        }
    }

    SECTION("the base pointer never moves across growth")
    {
        r.ensure(1);
        auto const* base = r.data();
        for (auto bytes = page; bytes <= 64 * page; bytes *= 2) {
            r.ensure(bytes);
            REQUIRE(r.data() == base);
        }
    }

    SECTION("contents survive growth")
    {
        r.ensure(page);
        std::memset(r.data(), 0x5A, page);
        r.ensure(32 * page);
        auto const* bytes = static_cast<unsigned char const*>(r.data());
        REQUIRE(bytes[0] == 0x5A);
        REQUIRE(bytes[page - 1] == 0x5A);
    }

    SECTION("ensure below the committed size is a no-op")
    {
        r.ensure(4 * page);
        auto const before = r.committed();
        r.ensure(1);
        REQUIRE(r.committed() == before);
    }
}

TEST_CASE("Region shrink", "[region]")
{
    auto const page = pup::platform::vm::page_size();
    auto r = Region {};

    SECTION("decommits the tail and keeps the prefix intact")
    {
        r.ensure(8 * page);
        auto* bytes = static_cast<unsigned char*>(r.data());
        std::memset(bytes, 0xC3, 8 * page);
        r.shrink(2 * page);
        REQUIRE(r.committed() == 2 * page);
        REQUIRE(bytes[0] == 0xC3);
        REQUIRE(bytes[2 * page - 1] == 0xC3);
    }

    SECTION("regrowth after shrink reads as zero")
    {
        r.ensure(4 * page);
        std::memset(r.data(), 0xC3, 4 * page);
        r.shrink(page);
        r.ensure(4 * page);
        auto const* bytes = static_cast<unsigned char const*>(r.data());
        REQUIRE(bytes[page] == 0);
        REQUIRE(bytes[4 * page - 1] == 0);
    }

    SECTION("shrink to zero releases the reservation")
    {
        r.ensure(4 * page);
        r.shrink(0);
        REQUIRE(r.data() == nullptr);
        REQUIRE(r.committed() == 0);
    }

    SECTION("shrink rounds the kept prefix up to a page")
    {
        r.ensure(4 * page);
        r.shrink(100);
        REQUIRE(r.committed() == page);
    }
}

TEST_CASE("Region move semantics", "[region]")
{
    auto const page = pup::platform::vm::page_size();

    SECTION("move construction transfers ownership")
    {
        auto a = Region {};
        a.ensure(page);
        std::memset(a.data(), 7, page);
        auto const* base = a.data();

        auto b = Region { std::move(a) };
        REQUIRE(b.data() == base);
        REQUIRE(b.committed() >= page);
        REQUIRE(a.data() == nullptr);
        REQUIRE(a.committed() == 0);
    }

    SECTION("move assignment transfers ownership")
    {
        auto a = Region {};
        a.ensure(page);
        auto const* base = a.data();

        auto b = Region {};
        b.ensure(2 * page);
        b = std::move(a);
        REQUIRE(b.data() == base);
        REQUIRE(a.data() == nullptr);
    }
}

TEST_CASE("Region custom reservation size", "[region]")
{
    auto const page = pup::platform::vm::page_size();
    auto r = Region { 16 * page };
    REQUIRE(r.reserved() == 0);
    r.ensure(16 * page);
    REQUIRE(r.committed() == 16 * page);
    REQUIRE(r.reserved() == 16 * page);
}
