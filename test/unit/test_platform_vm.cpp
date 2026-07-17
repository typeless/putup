// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/platform/vm.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vm = pup::platform::vm;

TEST_CASE("vm page size", "[platform][vm]")
{
    auto const page = vm::page_size();

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

TEST_CASE("vm reserve and release", "[platform][vm]")
{
    auto const page = vm::page_size();

    SECTION("reserve returns a page-aligned address")
    {
        auto* base = vm::reserve(16 * page);
        REQUIRE(base != nullptr);
        REQUIRE(reinterpret_cast<std::uintptr_t>(base) % page == 0);
        vm::release(base, 16 * page);
    }

    SECTION("a large reservation succeeds without committing memory")
    {
        auto const gib = std::size_t { 1 } << 30;
        auto* base = vm::reserve(4 * gib);
        REQUIRE(base != nullptr);
        vm::release(base, 4 * gib);
    }
}

TEST_CASE("vm commit", "[platform][vm]")
{
    auto const page = vm::page_size();
    auto* base = vm::reserve(16 * page);
    REQUIRE(base != nullptr);

    SECTION("committed pages are readable, writable, and zero-filled")
    {
        vm::commit(base, 2 * page);
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
        vm::commit(lo, page);
        vm::commit(hi, page);
        lo[0] = 1;
        hi[page - 1] = 2;
        REQUIRE(lo[0] == 1);
        REQUIRE(hi[page - 1] == 2);
    }

    SECTION("commit is idempotent and preserves contents")
    {
        vm::commit(base, page);
        auto* bytes = static_cast<unsigned char*>(base);
        bytes[7] = 42;
        vm::commit(base, page);
        REQUIRE(bytes[7] == 42);
    }

    vm::release(base, 16 * page);
}

TEST_CASE("vm decommit", "[platform][vm]")
{
    auto const page = vm::page_size();
    auto* base = vm::reserve(16 * page);
    REQUIRE(base != nullptr);

    SECTION("recommitted pages read as zero again")
    {
        vm::commit(base, 2 * page);
        auto* bytes = static_cast<unsigned char*>(base);
        std::memset(bytes, 0xCD, 2 * page);
        vm::decommit(base, 2 * page);
        vm::commit(base, 2 * page);
        for (std::size_t i = 0; i < 2 * page; ++i) {
            REQUIRE(bytes[i] == 0);
        }
    }

    SECTION("decommitting a tail subrange leaves the head intact")
    {
        vm::commit(base, 4 * page);
        auto* bytes = static_cast<unsigned char*>(base);
        std::memset(bytes, 0xEF, 4 * page);
        vm::decommit(bytes + 2 * page, 2 * page);
        REQUIRE(bytes[0] == 0xEF);
        REQUIRE(bytes[2 * page - 1] == 0xEF);
    }

    vm::release(base, 16 * page);
}
