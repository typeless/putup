// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "tiled/pool.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <span>
#include <vector>

using tiled::Pool;

namespace {

constexpr auto POOL_LOG2 = std::uint8_t { 10 };
constexpr auto MIN_LOG2 = std::uint8_t { 4 };

auto block_bytes(std::uint8_t log2) -> std::uint64_t
{
    return std::uint64_t { 1 } << log2;
}

auto overlaps(std::uint64_t a, std::uint8_t a_log2, std::uint64_t b, std::uint8_t b_log2) -> bool
{
    return a < b + block_bytes(b_log2) && b < a + block_bytes(a_log2);
}

auto has_free_slot(std::map<std::uint64_t, std::uint8_t> const& live, std::uint8_t want) -> bool
{
    for (auto slot = std::uint64_t { 0 }; slot < block_bytes(POOL_LOG2); slot += block_bytes(want)) {
        auto free = true;
        for (auto const& [offset, log2] : live) {
            if (overlaps(slot, want, offset, log2)) {
                free = false;
                break;
            }
        }
        if (free) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("Pool construction validates its geometry once", "[tiled][pool]")
{
    auto metadata = std::vector<std::uint8_t>(Pool::metadata_bytes(POOL_LOG2, MIN_LOG2));

    REQUIRE(Pool::make(metadata, POOL_LOG2, MIN_LOG2).has_value());

    SECTION("metadata one byte short or long")
    {
        REQUIRE_FALSE(Pool::make(std::span(metadata).first(metadata.size() - 1), POOL_LOG2, MIN_LOG2).has_value());
        auto longer = std::vector<std::uint8_t>(metadata.size() + 1);
        REQUIRE_FALSE(Pool::make(longer, POOL_LOG2, MIN_LOG2).has_value());
    }
    SECTION("leaf larger than the pool")
    {
        auto two = std::array<std::uint8_t, 2> {};
        REQUIRE_FALSE(Pool::make(two, 3, 4).has_value());
        REQUIRE_FALSE(Pool::make(std::span<std::uint8_t> {}, 3, 4).has_value());
    }
    SECTION("pool of 2^64 bytes, with metadata of the size the formula gives")
    {
        REQUIRE(Pool::metadata_bytes(64, 58) == metadata.size());
        REQUIRE_FALSE(Pool::make(metadata, 64, 58).has_value());
    }
    SECTION("depth whose node count does not fit in 64 bits")
    {
        REQUIRE(Pool::metadata_bytes(63, 0) == 0);
        REQUIRE_FALSE(Pool::make(std::span<std::uint8_t> {}, 63, 0).has_value());
    }
}

TEST_CASE("Pool hands out offset 0 first on zeroed metadata", "[tiled][pool]")
{
    auto metadata = std::vector<std::uint8_t>(Pool::metadata_bytes(POOL_LOG2, MIN_LOG2));
    auto pool = *Pool::make(metadata, POOL_LOG2, MIN_LOG2);

    for (auto log2 = MIN_LOG2; log2 <= POOL_LOG2; ++log2) {
        CAPTURE(log2);
        std::fill(metadata.begin(), metadata.end(), std::uint8_t { 0 });
        REQUIRE(pool.acquire(log2) == std::uint64_t { 0 });
    }
}

TEST_CASE("Pool refuses everything once the whole pool is taken", "[tiled][pool]")
{
    auto metadata = std::vector<std::uint8_t>(Pool::metadata_bytes(POOL_LOG2, MIN_LOG2));
    auto pool = *Pool::make(metadata, POOL_LOG2, MIN_LOG2);

    REQUIRE(pool.acquire(POOL_LOG2) == std::uint64_t { 0 });
    REQUIRE_FALSE(pool.acquire(MIN_LOG2).has_value());
    REQUIRE(pool.release(0, POOL_LOG2));
    REQUIRE(pool.acquire(MIN_LOG2) == std::uint64_t { 0 });
}

TEST_CASE("Pool with a single leaf", "[tiled][pool]")
{
    auto metadata = std::array<std::uint8_t, 2> {};
    auto pool = *Pool::make(metadata, MIN_LOG2, MIN_LOG2);

    REQUIRE(pool.acquire(MIN_LOG2) == std::uint64_t { 0 });
    REQUIRE_FALSE(pool.acquire(MIN_LOG2).has_value());
    REQUIRE(pool.release(0, MIN_LOG2));
    REQUIRE(metadata == std::array<std::uint8_t, 2> {});
}

TEST_CASE("Pool fills a smaller free block before splitting a larger one", "[tiled][pool]")
{
    auto metadata = std::vector<std::uint8_t>(Pool::metadata_bytes(POOL_LOG2, MIN_LOG2));
    auto pool = *Pool::make(metadata, POOL_LOG2, MIN_LOG2);
    auto const half = static_cast<std::uint8_t>(POOL_LOG2 - 1);
    auto const quarter = static_cast<std::uint8_t>(POOL_LOG2 - 2);

    REQUIRE(pool.acquire(half) == std::uint64_t { 0 });
    REQUIRE(pool.acquire(quarter) == block_bytes(half));
    REQUIRE(pool.release(0, half));

    REQUIRE(pool.acquire(quarter) == block_bytes(half) + block_bytes(quarter));
    REQUIRE(pool.acquire(half) == std::uint64_t { 0 });
}

TEST_CASE("Pool refuses sizes outside its range", "[tiled][pool]")
{
    auto metadata = std::vector<std::uint8_t>(Pool::metadata_bytes(POOL_LOG2, MIN_LOG2));
    auto pool = *Pool::make(metadata, POOL_LOG2, MIN_LOG2);

    REQUIRE_FALSE(pool.acquire(MIN_LOG2 - 1).has_value());
    REQUIRE_FALSE(pool.acquire(POOL_LOG2 + 1).has_value());
}

TEST_CASE("Pool reports a release that does not name a live block", "[tiled][pool]")
{
    auto metadata = std::vector<std::uint8_t>(Pool::metadata_bytes(POOL_LOG2, MIN_LOG2));
    auto pool = *Pool::make(metadata, POOL_LOG2, MIN_LOG2);
    auto const a = *pool.acquire(6);
    auto const b = *pool.acquire(6);

    SECTION("wrong size")
    {
        REQUIRE_FALSE(pool.release(a, 7));
        REQUIRE_FALSE(pool.release(a, 5));
    }
    SECTION("interior offset")
    {
        REQUIRE_FALSE(pool.release(a + block_bytes(MIN_LOG2), MIN_LOG2));
    }
    SECTION("double release")
    {
        REQUIRE(pool.release(b, 6));
        REQUIRE_FALSE(pool.release(b, 6));
    }
    SECTION("never acquired")
    {
        REQUIRE_FALSE(pool.release(block_bytes(POOL_LOG2) / 2, 6));
    }
    SECTION("outside the pool, where the node index would alias a block inside it")
    {
        REQUIRE(pool.release(a, 6));
        REQUIRE(pool.release(b, 6));
        REQUIRE(pool.acquire(5) == std::uint64_t { 0 });
        REQUIRE_FALSE(pool.release(block_bytes(POOL_LOG2), 6));
        REQUIRE(pool.release(0, 5));
    }
    SECTION("larger than the pool")
    {
        REQUIRE_FALSE(pool.release(0, POOL_LOG2 + 1));
    }
}

TEST_CASE("Pool refuses a release below the leaf size without reading past its metadata", "[tiled][pool]")
{
    auto const bytes = Pool::metadata_bytes(POOL_LOG2, MIN_LOG2);
    auto backing = std::vector<std::uint8_t>(2 * bytes, std::uint8_t { 0xFF });
    std::fill(backing.begin(), backing.begin() + static_cast<long>(bytes), std::uint8_t { 0 });
    auto pool = *Pool::make(std::span(backing).first(bytes), POOL_LOG2, MIN_LOG2);

    REQUIRE_FALSE(pool.release(0, MIN_LOG2 - 1));
}

TEST_CASE("Pool agrees with a set of disjoint aligned blocks over random sequences", "[tiled][pool]")
{
    for (auto seed = 1U; seed <= 50; ++seed) {
        CAPTURE(seed);
        auto rng = std::mt19937 { seed };
        auto metadata = std::vector<std::uint8_t>(Pool::metadata_bytes(POOL_LOG2, MIN_LOG2));
        auto pool = *Pool::make(metadata, POOL_LOG2, MIN_LOG2);
        auto live = std::map<std::uint64_t, std::uint8_t> {};

        for (auto step = 0; step < 400; ++step) {
            CAPTURE(step);
            auto const roll = rng() % 4;
            if (roll < 2) {
                auto const want = static_cast<std::uint8_t>(MIN_LOG2 - 1 + rng() % (POOL_LOG2 - MIN_LOG2 + 3));
                auto const got = pool.acquire(want);
                auto const in_range = want >= MIN_LOG2 && want <= POOL_LOG2;
                if (!got) {
                    REQUIRE((!in_range || !has_free_slot(live, want)));
                    continue;
                }
                REQUIRE(in_range);
                REQUIRE(*got % block_bytes(want) == 0);
                REQUIRE(*got + block_bytes(want) <= block_bytes(POOL_LOG2));
                for (auto const& [offset, log2] : live) {
                    REQUIRE_FALSE(overlaps(*got, want, offset, log2));
                }
                live.emplace(*got, want);
            } else if (roll == 2 && !live.empty()) {
                auto it = live.begin();
                std::advance(it, static_cast<long>(rng() % live.size()));
                REQUIRE(pool.release(it->first, it->second));
                live.erase(it);
            } else {
                auto const wide = rng() % 2 == 0;
                auto const offset = wide ? (std::uint64_t { rng() } << 32 | rng()) : std::uint64_t { rng() % (block_bytes(POOL_LOG2) + 64) };
                auto const log2 = wide ? static_cast<std::uint8_t>(rng() % 256)
                                       : static_cast<std::uint8_t>(MIN_LOG2 - 1 + rng() % (POOL_LOG2 - MIN_LOG2 + 3));
                auto const it = live.find(offset);
                auto const expected = it != live.end() && it->second == log2;
                REQUIRE(pool.release(offset, log2) == expected);
                if (expected) {
                    live.erase(it);
                }
            }
        }

        for (auto const& [offset, log2] : live) {
            REQUIRE(pool.release(offset, log2));
        }
        for (auto const byte : metadata) {
            REQUIRE(byte == 0);
        }
    }
}
