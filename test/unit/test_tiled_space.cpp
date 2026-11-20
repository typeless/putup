// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "tiled/layout.hpp"
#include "tiled/platform.hpp"
#include "tiled/pool.hpp"
#include "tiled/space_storage.hpp"
#include "tiled/tile.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

using tiled::Layout;
using tiled::Tile;

namespace {

constexpr auto putup_like = Layout {
    .root_log2 = 47,
    .image_nodes = { { 0, 33 }, { std::uint64_t { 1 } << 46, 46 } },
    .base = std::uint64_t { 1 } << 45,
    .pool_log2 = 44,
    .heap_log2 = 34,
    .min_tile_log2 = 24,
};

constexpr auto with(Layout layout, auto change) -> Layout
{
    change(layout);
    return layout;
}

constexpr auto test_layout = Layout {
    .root_log2 = 47,
    .image_nodes = { { 0, 33 }, { std::uint64_t { 1 } << 46, 46 } },
    .base = std::uint64_t { 0x3000'0000'0000 },
    .pool_log2 = 32,
    .heap_log2 = 28,
    .min_tile_log2 = 24,
};

constinit tiled::SpaceStorage<test_layout> test_space; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

auto address_of(std::uint64_t offset) -> std::byte*
{
    return reinterpret_cast<std::byte*>(test_layout.base + offset);
}

auto pool_is_empty(tiled::Pool& pool) -> bool
{
    auto const whole = pool.acquire(test_layout.pool_log2);
    return whole.has_value() && pool.release(*whole, test_layout.pool_log2);
}

} // namespace

TEST_CASE("SpaceStorage can be neither copied nor moved", "[tiled][space]")
{
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tiled::SpaceStorage<test_layout>>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<tiled::SpaceStorage<test_layout>>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<tiled::SpaceStorage<test_layout>>);
}

TEST_CASE("Layout validity is checked at compile time", "[tiled][space]")
{
    STATIC_REQUIRE(tiled::is_valid(putup_like));
    STATIC_REQUIRE(tiled::is_valid(test_layout));

    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.root_log2 = 64; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.heap_log2 = 44; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.heap_log2 = 23; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.min_tile_log2 = 45; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.base += std::uint64_t { 1 } << 43; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.base = std::uint64_t { 1 } << 47; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.image_nodes[0].base = 4096; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.image_nodes[1] = { std::uint64_t { 1 } << 47, 47 }; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.image_nodes[1] = { std::uint64_t { 1 } << 45, 45 }; })));
    STATIC_REQUIRE_FALSE(tiled::is_valid(with(putup_like, [](Layout& l) { l.image_nodes[0] = { 0, 46 }; })));
}

TEST_CASE("SpaceStorage realizes one pool at the declared base", "[tiled][space]")
{
    auto& pool = test_space.pool();
    REQUIRE(&test_space.pool() == &pool);
    REQUIRE_FALSE(tiled::reserve_at(address_of(0), std::size_t { 1 } << test_layout.pool_log2));

    auto const tile = Tile::acquire(pool, 24);
    REQUIRE(tile.size_log2() == 24);
    auto const address = reinterpret_cast<std::uintptr_t>(tile.base());
    REQUIRE(address >= test_layout.base);
    REQUIRE(address + (std::uintptr_t { 1 } << 24) <= test_layout.base + (std::uint64_t { 1 } << test_layout.pool_log2));
    REQUIRE(address % (std::uintptr_t { 1 } << 24) == 0);
}

TEST_CASE("Tile returns its block to the pool exactly once", "[tiled][space]")
{
    auto& pool = test_space.pool();

    SECTION("when it is destroyed")
    {
        {
            auto const tile = Tile::acquire(pool, test_layout.pool_log2);
            REQUIRE(tile.base() == address_of(0));
            REQUIRE_FALSE(pool.acquire(test_layout.min_tile_log2).has_value());
        }
        REQUIRE(pool_is_empty(pool));
    }
    SECTION("two live tiles do not overlap")
    {
        auto const a = Tile::acquire(pool, 25);
        auto const b = Tile::acquire(pool, 25);
        REQUIRE(a.base() != b.base());
        auto const distance = a.base() < b.base() ? b.base() - a.base() : a.base() - b.base();
        REQUIRE(distance >= std::ptrdiff_t { 1 } << 25);
    }
}

TEST_CASE("Tile decommits its memory when destroyed", "[tiled][space]")
{
    auto& pool = test_space.pool();
    auto const page = tiled::page_size();
    {
        auto const tile = Tile::acquire(pool, test_layout.min_tile_log2);
        REQUIRE(tile.base() == address_of(0));
        tiled::commit(tile.base(), page);
        tile.base()[page - 1] = std::byte { 0x5A };
    }
    auto const again = Tile::acquire(pool, test_layout.min_tile_log2);
    REQUIRE(again.base() == address_of(0));
    tiled::commit(again.base(), page);
    REQUIRE(again.base()[page - 1] == std::byte { 0 });
}
