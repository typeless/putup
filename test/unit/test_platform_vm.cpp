// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/mem/bump_alloc.hpp"
#include "pup/core/memory_layout.hpp"
#include "tiled/platform.hpp"
#include "tiled/tile.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

TEST_CASE("vm page size", "[platform][vm]")
{
    auto const page = tiled::page_size();

    SECTION("is a power of two")
    {
        REQUIRE(page > 0);
        REQUIRE((page & (page - 1)) == 0);
    }

    SECTION("is at least 4 KiB")
    {
        REQUIRE(page >= 4096);
    }
}

TEST_CASE("reserve_at reserves exactly the requested address or refuses", "[platform][vm]")
{
    auto const page = tiled::page_size();
    auto const bytes = 16 * page;

    SECTION("a free range is reserved at that address and can be committed")
    {
        auto* const address = reinterpret_cast<void*>(std::uintptr_t { 0x3800'0000'0000 });
        REQUIRE(tiled::reserve_at(address, bytes));
        tiled::commit(address, page);
        auto* first = static_cast<unsigned char*>(address);
        REQUIRE(first[0] == 0);
        first[page - 1] = 0xAB;
        REQUIRE(first[page - 1] == 0xAB);
    }

    SECTION("a range that is already reserved is refused, whole or in part")
    {
        auto* const address = reinterpret_cast<void*>(std::uintptr_t { 0x3900'0000'0000 });
        REQUIRE(tiled::reserve_at(address, bytes));
        REQUIRE_FALSE(tiled::reserve_at(address, bytes));
        REQUIRE_FALSE(tiled::reserve_at(static_cast<char*>(address) + page, page));
    }
}

TEST_CASE("vm commit", "[platform][vm]")
{
    auto const page = tiled::page_size();
    auto const tile = tiled::Tile::acquire(pup::mem::process_pool(), pup::memory_layout.min_tile_log2);
    auto* base = static_cast<void*>(tile.base());

    SECTION("committed pages are readable, writable, and zero-filled")
    {
        tiled::commit(base, 2 * page);
        auto* bytes = static_cast<unsigned char*>(base);
        for (std::size_t i = 0; i < 2 * page; ++i) {
            REQUIRE(bytes[i] == 0);
        }
        std::memset(bytes, 0xAB, 2 * page);
        REQUIRE(bytes[0] == 0xAB);
        REQUIRE(bytes[2 * page - 1] == 0xAB);
    }

    SECTION("disjoint subranges of one reservation commit independently")
    {
        auto* lo = static_cast<unsigned char*>(base);
        auto* hi = lo + 8 * page;
        tiled::commit(lo, page);
        tiled::commit(hi, page);
        lo[0] = 1;
        hi[page - 1] = 2;
        REQUIRE(lo[0] == 1);
        REQUIRE(hi[page - 1] == 2);
    }

    SECTION("commit is idempotent and preserves contents")
    {
        tiled::commit(base, page);
        auto* bytes = static_cast<unsigned char*>(base);
        bytes[7] = 42;
        tiled::commit(base, page);
        REQUIRE(bytes[7] == 42);
    }
}

TEST_CASE("vm decommit", "[platform][vm]")
{
    auto const page = tiled::page_size();
    auto const tile = tiled::Tile::acquire(pup::mem::process_pool(), pup::memory_layout.min_tile_log2);
    auto* base = static_cast<void*>(tile.base());

    SECTION("recommitted pages read as zero again")
    {
        tiled::commit(base, 2 * page);
        auto* bytes = static_cast<unsigned char*>(base);
        std::memset(bytes, 0xCD, 2 * page);
        tiled::decommit(base, 2 * page);
        tiled::commit(base, 2 * page);
        for (std::size_t i = 0; i < 2 * page; ++i) {
            REQUIRE(bytes[i] == 0);
        }
    }

    SECTION("decommitting a tail subrange leaves the head intact")
    {
        tiled::commit(base, 4 * page);
        auto* bytes = static_cast<unsigned char*>(base);
        std::memset(bytes, 0xEF, 4 * page);
        tiled::decommit(bytes + 2 * page, 2 * page);
        REQUIRE(bytes[0] == 0xEF);
        REQUIRE(bytes[2 * page - 1] == 0xEF);
    }
}
