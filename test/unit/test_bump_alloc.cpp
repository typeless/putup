// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/vec.hpp"
#include "pup/mem/bump_alloc.hpp"
#include "pup/mem/memory_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

auto in_pool_tile(void const* p) -> bool
{
    auto const address = reinterpret_cast<std::uintptr_t>(p);
    auto const base = pup::mem::memory_layout.base;
    return address >= base + (std::uint64_t { 1 } << pup::mem::memory_layout.heap_log2)
        && address - base < (std::uint64_t { 1 } << pup::mem::memory_layout.pool_log2);
}

auto in_heap_node(void const* p) -> bool
{
    auto const address = reinterpret_cast<std::uintptr_t>(p);
    auto const base = pup::mem::memory_layout.base;
    return address >= base && address - base < (std::uint64_t { 1 } << pup::mem::memory_layout.heap_log2);
}

} // namespace

TEST_CASE("bump_alloc returns memory inside the declared heap node", "[bump]")
{
    REQUIRE(in_heap_node(pup::mem::bump_alloc(1, 1)));
    REQUIRE(in_heap_node(pup::mem::bump_alloc(std::size_t { 1 } << 12, 64)));
}

TEST_CASE("bump_allocated_bytes is the offset of the heap's top", "[bump]")
{
    auto* const p = static_cast<std::byte*>(pup::mem::bump_alloc(24, 8));
    auto const base = reinterpret_cast<std::byte*>(pup::mem::memory_layout.base);
    REQUIRE(pup::mem::bump_allocated_bytes() == static_cast<std::size_t>(p + 24 - base));
}

TEST_CASE("growing a Vec that is not on top of the heap counts its old bytes as copied", "[bump]")
{
    auto v = pup::Vec<std::uint32_t> {};
    v.reserve(4);
    for (auto i = std::uint32_t { 0 }; i < 4; ++i) {
        v.push_back(i);
    }
    pup::mem::bump_alloc(1, 1);
    auto const before = pup::mem::copied_bytes();

    v.reserve(64);

    REQUIRE(pup::mem::copied_bytes() - before == 4 * sizeof(std::uint32_t));
}

TEST_CASE("relocate copies the bytes and adds them to the copied count", "[bump]")
{
    auto const source = std::array<std::uint8_t, 5> { 1, 2, 3, 4, 5 };
    auto target = std::array<std::uint8_t, 5> {};
    auto const before = pup::mem::copied_bytes();

    pup::mem::relocate(target.data(), source.data(), source.size());

    REQUIRE(target == source);
    REQUIRE(pup::mem::copied_bytes() - before == source.size());
}

TEST_CASE("growing a Vec on top of the heap copies nothing", "[bump]")
{
    auto v = pup::Vec<std::uint32_t> {};
    v.reserve(4);
    v.push_back(1);
    auto const before = pup::mem::copied_bytes();

    v.reserve(64);

    REQUIRE(pup::mem::copied_bytes() == before);
}

TEST_CASE("a large bump_alloc lands on its own pool tile outside the heap", "[bump]")
{
    auto* const p = pup::mem::bump_alloc(std::size_t { 1 } << 20, 64);
    REQUIRE(in_pool_tile(p));
    pup::mem::bump_try_pop(p, std::size_t { 1 } << 20);
}

TEST_CASE("a spilled Vec that is not on top grows in place without copies", "[bump]")
{
    auto v = pup::Vec<std::uint64_t> {};
    while (v.size() < (std::size_t { 1 } << 16)) {
        v.push_back(v.size());
        pup::mem::bump_alloc(1, 1);
    }
    REQUIRE(in_pool_tile(v.data()));
    auto const* const spilled = v.data();
    auto const before = pup::mem::copied_bytes();

    while (v.size() < (std::size_t { 1 } << 20)) {
        v.push_back(v.size());
        pup::mem::bump_alloc(1, 1);
    }

    REQUIRE(v.data() == spilled);
    REQUIRE(pup::mem::copied_bytes() == before);
    REQUIRE(v[12345] == 12345);
}

TEST_CASE("destroying a spilled Vec returns its tile to the pool", "[bump]")
{
    auto const bytes = std::size_t { 1 } << 20;
    void const* first = nullptr;
    {
        auto v = pup::Vec<std::byte> {};
        v.resize(bytes);
        first = v.data();
        REQUIRE(in_pool_tile(first));
    }
    auto w = pup::Vec<std::byte> {};
    w.resize(bytes);
    REQUIRE(w.data() == first);
}

TEST_CASE("a Vec on top of the heap moves to its own tile before it reaches the spill size", "[bump]")
{
    auto v = pup::Vec<std::uint64_t> {};
    while (v.size() < (std::size_t { 1 } << 12)) {
        v.push_back(v.size());
    }

    REQUIRE(in_pool_tile(v.data()));
}
