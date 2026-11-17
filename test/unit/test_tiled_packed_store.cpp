// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "tiled/layout.hpp"
#include "tiled/packed_store.hpp"
#include "tiled/space_storage.hpp"

#include <cstdint>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr auto packed_layout = tiled::Layout {
    .root_log2 = 47,
    .image_nodes = { { 0, 33 }, { std::uint64_t { 1 } << 46, 46 } },
    .base = std::uint64_t { 0x3200'0000'0000 },
    .pool_log2 = 30,
    .heap_log2 = 26,
    .min_tile_log2 = 24,
};

constinit tiled::SpaceStorage<packed_layout> packed_space; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

constexpr auto STORE_LOG2 = std::uint8_t { 24 };

struct Head {
    std::uint8_t tag;
    std::uint8_t low;
    std::uint8_t high;

    auto operator==(Head const&) const -> bool = default;
};

struct Pair {
    std::uint32_t first;
    std::uint16_t second;
    std::uint16_t third;

    auto operator==(Pair const&) const -> bool = default;
};

struct Padded {
    std::uint32_t wide;
    std::uint8_t narrow;
};

using Store = tiled::PackedStore<Head, std::uint32_t, Pair>;
using First = Store::Builder<0>;
using Last = Store::Builder<1>;

auto fresh_store() -> Store
{
    return Store { packed_space.pool(), STORE_LOG2 };
}

struct Expected {
    Head head;
    std::vector<std::uint32_t> ids;
    std::vector<Pair> pairs;
};

auto random_record(std::mt19937& rng) -> Expected
{
    auto record = Expected {
        .head = { static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng()) },
        .ids = {},
        .pairs = {},
    };
    auto const long_run = rng() % 8 == 0;
    auto const id_count = long_run ? 4096 + (rng() % 4096) : rng() % 6;
    for (auto i = 0U; i < id_count; ++i) {
        record.ids.push_back(static_cast<std::uint32_t>(rng()));
    }
    auto const pair_count = rng() % 6;
    for (auto i = 0U; i < pair_count; ++i) {
        record.pairs.push_back({ static_cast<std::uint32_t>(rng()), static_cast<std::uint16_t>(rng()), static_cast<std::uint16_t>(rng()) });
    }
    return record;
}

auto build(Store& store, Expected const& record) -> tiled::RecordId
{
    auto ids = store.open(record.head);
    for (auto id : record.ids) {
        ids.push(id);
    }
    auto pairs = std::move(ids).next();
    for (auto const& pair : record.pairs) {
        pairs.push(pair);
    }
    return std::move(pairs).close();
}

template<typename Builder, typename Element>
concept Pushes = requires(Builder b, Element e) { b.push(e); };

template<typename Builder>
concept Advances = requires(Builder b) { std::move(b).next(); };

template<typename Builder>
concept AdvancesFromLvalue = requires(Builder b) { b.next(); };

template<typename Builder>
concept Closes = requires(Builder b) { std::move(b).close(); };

template<typename Builder>
concept ClosesFromLvalue = requires(Builder b) { b.close(); };

template<typename H, typename... E>
concept Storable = requires { typename tiled::PackedStore<H, E...>; };

} // namespace

TEST_CASE("A packed record reads back the head and every sequence it was built with", "[tiled][packed]")
{
    for (auto seed = 1U; seed <= 40; ++seed) {
        CAPTURE(seed);
        auto rng = std::mt19937 { seed };
        auto store = fresh_store();
        auto expected = std::vector<Expected> {};
        auto ids = std::vector<tiled::RecordId> {};
        auto const record_count = 1 + (rng() % 12);
        for (auto i = 0U; i < record_count; ++i) {
            expected.push_back(random_record(rng));
            ids.push_back(build(store, expected.back()));
        }
        for (auto i = 0U; i < record_count; ++i) {
            auto const view = store.view(ids[i]);
            REQUIRE(view.head() == expected[i].head);
            auto const id_run = view.sequence<0>();
            REQUIRE(id_run.size() == expected[i].ids.size());
            for (auto k = std::size_t { 0 }; k < id_run.size(); ++k) {
                REQUIRE(id_run[k] == expected[i].ids[k]);
            }
            auto read_pairs = std::vector<Pair> {};
            for (auto pair : view.sequence<1>()) {
                read_pairs.push_back(pair);
            }
            REQUIRE(read_pairs == expected[i].pairs);
        }
    }
}

TEST_CASE("Packed records follow one another with no bytes between them", "[tiled][packed]")
{
    auto store = fresh_store();
    auto const first = build(store, { .head = { 1, 2, 3 }, .ids = { 7, 8 }, .pairs = { { 9, 10, 11 } } });
    auto const second = build(store, { .head = { 4, 5, 6 }, .ids = { 42 }, .pairs = {} });
    auto const third = build(store, { .head = { 7, 8, 9 }, .ids = {}, .pairs = {} });

    REQUIRE(first.offset() == 0);
    REQUIRE(second.offset() == sizeof(Head) + 4 + (2 * sizeof(std::uint32_t)) + 4 + sizeof(Pair));
    REQUIRE(second.offset() % 2 == 1);
    REQUIRE(third.offset() == second.offset() + sizeof(Head) + 4 + 4 + 4);
    REQUIRE(store.view(second).sequence<0>()[0] == 42);
    REQUIRE(store.view(third).head() == Head { 7, 8, 9 });
}

TEST_CASE("A dropped build gives its bytes back to the next record", "[tiled][packed]")
{
    auto store = fresh_store();
    auto const first = build(store, { .head = { 1, 1, 1 }, .ids = { 1 }, .pairs = {} });
    {
        auto abandoned = store.open({ 2, 2, 2 });
        abandoned.push(5);
        abandoned.push(6);
    }
    auto const next = build(store, { .head = { 3, 3, 3 }, .ids = {}, .pairs = { { 1, 2, 3 } } });

    REQUIRE(next.offset() == first.offset() + sizeof(Head) + 4 + 4 + 4);
    REQUIRE(store.view(next).head() == Head { 3, 3, 3 });
    REQUIRE(store.view(next).sequence<1>()[0] == Pair { 1, 2, 3 });
}

TEST_CASE("A build is intact until another record opens on its store", "[tiled][packed]")
{
    auto store = fresh_store();
    auto outer = store.open({ 1, 2, 3 });
    outer.push(1);
    REQUIRE(outer.intact());

    auto inner = store.open({ 4, 5, 6 });
    REQUIRE_FALSE(outer.intact());
    REQUIRE(inner.intact());
}

TEST_CASE("A build is intact across the pages it commits", "[tiled][packed]")
{
    auto store = fresh_store();
    auto ids = store.open({ 0, 0, 0 });
    for (auto i = 0U; i < 20000; ++i) {
        ids.push(i);
        REQUIRE(ids.intact());
    }
    auto pairs = std::move(ids).next();
    REQUIRE(pairs.intact());
    auto const id = std::move(pairs).close();
    REQUIRE(store.view(id).sequence<0>().size() == 20000);
    REQUIRE(store.view(id).sequence<0>()[19999] == 19999);
}

TEST_CASE("A builder accepts only the open sequence's element, in order", "[tiled][packed]")
{
    STATIC_REQUIRE(Pushes<First, std::uint32_t>);
    STATIC_REQUIRE_FALSE(Pushes<First, Pair>);
    STATIC_REQUIRE(Pushes<Last, Pair>);
    STATIC_REQUIRE_FALSE(Pushes<Last, std::uint32_t>);

    STATIC_REQUIRE(Advances<First>);
    STATIC_REQUIRE_FALSE(AdvancesFromLvalue<First>);
    STATIC_REQUIRE_FALSE(Advances<Last>);
    STATIC_REQUIRE_FALSE(AdvancesFromLvalue<Last>);

    STATIC_REQUIRE_FALSE(Closes<First>);
    STATIC_REQUIRE_FALSE(ClosesFromLvalue<First>);
    STATIC_REQUIRE(Closes<Last>);
    STATIC_REQUIRE_FALSE(ClosesFromLvalue<Last>);

    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<First>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<First>);
}

TEST_CASE("A packed store takes only parts without padding", "[tiled][packed]")
{
    STATIC_REQUIRE(Storable<Head, std::uint32_t, Pair>);
    STATIC_REQUIRE_FALSE(Storable<Padded, std::uint32_t>);
    STATIC_REQUIRE_FALSE(Storable<Head, Padded>);
    STATIC_REQUIRE_FALSE(Storable<Head>);
}
