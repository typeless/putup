// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "tiled/stack_cursor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using tiled::StackCursor;

namespace {

constexpr auto PAGE = std::uint64_t { 64 };
constexpr auto RESERVED = PAGE * 16;
constexpr auto MAX = std::numeric_limits<std::uint64_t>::max();

auto round_up(std::uint64_t n, std::uint64_t to) -> std::uint64_t
{
    return (n + to - 1) / to * to;
}

auto holds_invariant(StackCursor const& c) -> bool
{
    return c.top() <= c.committed() && c.committed() <= RESERVED && c.committed() % PAGE == 0;
}

auto unchanged(StackCursor const& now, StackCursor const& before) -> bool
{
    return now.top() == before.top() && now.committed() == before.committed();
}

struct Block {
    std::uint64_t offset;
    std::uint64_t bytes;
};

} // namespace

TEST_CASE("StackCursor construction validates page and reservation", "[tiled][stack]")
{
    REQUIRE(StackCursor::make(RESERVED, PAGE).has_value());
    REQUIRE_FALSE(StackCursor::make(RESERVED, 0).has_value());
    REQUIRE_FALSE(StackCursor::make(0, 0).has_value());
    REQUIRE_FALSE(StackCursor::make(96, 48).has_value());
    REQUIRE_FALSE(StackCursor::make(RESERVED + 1, PAGE).has_value());

    auto const fresh = *StackCursor::make(RESERVED, PAGE);
    REQUIRE(fresh.top() == 0);
    REQUIRE(fresh.committed() == 0);
}

TEST_CASE("StackCursor push boundaries", "[tiled][stack]")
{
    auto cursor = *StackCursor::make(RESERVED, PAGE);

    SECTION("a push ending at the reservation fits; one byte more does not")
    {
        auto other = cursor;
        REQUIRE_FALSE(other.push(RESERVED + 1, 1).has_value());
        REQUIRE(unchanged(other, cursor));
        REQUIRE(cursor.push(RESERVED, 1).has_value());
    }
    SECTION("ending at the commit mark keeps it; one byte more adds one page")
    {
        REQUIRE(cursor.push(PAGE, 1).has_value());
        REQUIRE(cursor.committed() == PAGE);
        REQUIRE(cursor.push(1, 1).has_value());
        REQUIRE(cursor.committed() == 2 * PAGE);
    }
    SECTION("a zero-byte push succeeds and commits nothing")
    {
        REQUIRE(cursor.push(0, 8).has_value());
        REQUIRE(cursor.committed() == 0);
    }
    SECTION("an alignment that is not a power of two is refused")
    {
        REQUIRE_FALSE(cursor.push(8, 0).has_value());
        REQUIRE_FALSE(cursor.push(8, 3).has_value());
    }
    SECTION("sizes near the integer maximum are refused, not wrapped")
    {
        REQUIRE(cursor.push(1, 1).has_value());
        auto const before = cursor;
        REQUIRE_FALSE(cursor.push(MAX, 1).has_value());
        REQUIRE_FALSE(cursor.push(MAX - 1, 1).has_value());
        REQUIRE_FALSE(cursor.push(1, std::uint64_t { 1 } << 63).has_value());
        REQUIRE(unchanged(cursor, before));
    }
}

TEST_CASE("StackCursor refuses alignment padding that would wrap past 2^64", "[tiled][stack]")
{
    auto cursor = *StackCursor::make(MAX - PAGE + 1, PAGE);
    REQUIRE(cursor.push((std::uint64_t { 1 } << 63) + 1, 1).has_value());
    auto const before = cursor;
    REQUIRE_FALSE(cursor.push(0, std::uint64_t { 1 } << 63).has_value());
    REQUIRE(unchanged(cursor, before));
}

TEST_CASE("StackCursor extend_top boundaries", "[tiled][stack]")
{
    auto cursor = *StackCursor::make(RESERVED, PAGE);
    auto const offset = *cursor.push(100, 8);
    auto const pushed = cursor;

    SECTION("to the reservation fits; one byte more does not")
    {
        REQUIRE_FALSE(cursor.extend_top(offset, 100, RESERVED - offset + 1));
        REQUIRE(cursor.extend_top(offset, 100, RESERVED - offset));
    }
    SECTION("a wrong old size is refused")
    {
        REQUIRE_FALSE(cursor.extend_top(offset, 99, 200));
        REQUIRE(unchanged(cursor, pushed));
    }
    SECTION("shrinking lowers the top and keeps the commit mark")
    {
        REQUIRE(cursor.extend_top(offset, 100, 10));
        REQUIRE(cursor.top() == offset + 10);
        REQUIRE(cursor.committed() == pushed.committed());
    }
    SECTION("a large new size at a non-zero offset is refused, not wrapped")
    {
        auto const second = *cursor.push(100, 8);
        REQUIRE(second != 0);
        REQUIRE_FALSE(cursor.extend_top(second, 100, MAX));
        REQUIRE_FALSE(cursor.extend_top(second, 100, MAX - second + 1));
    }
    SECTION("an offset above the top is refused even when top - offset wraps to the size")
    {
        auto one = *StackCursor::make(RESERVED, PAGE);
        REQUIRE(one.push(1, 1).has_value());
        auto const before = one;
        REQUIRE_FALSE(one.extend_top(MAX, 2, 5));
        one.pop_top(MAX, 2);
        REQUIRE(unchanged(one, before));
    }
}

TEST_CASE("StackCursor decommit_above boundaries", "[tiled][stack]")
{
    auto used = *StackCursor::make(RESERVED, PAGE);
    REQUIRE(used.push(5 * PAGE, 1).has_value());
    used.pop_top(0, 5 * PAGE);
    auto const five_pages = used;

    SECTION("keep at or above the mark changes nothing")
    {
        used.decommit_above(used.committed());
        REQUIRE(unchanged(used, five_pages));
        used.decommit_above(MAX);
        REQUIRE(unchanged(used, five_pages));
    }
    SECTION("keep rounds up to a page")
    {
        used.decommit_above(PAGE + 1);
        REQUIRE(used.committed() == 2 * PAGE);
    }
    SECTION("keep zero with an empty stack decommits everything")
    {
        used.decommit_above(0);
        REQUIRE(used.committed() == 0);
    }
    SECTION("the mark never falls below the page-rounded top")
    {
        auto live = *StackCursor::make(RESERVED, PAGE);
        REQUIRE(live.push(3 * PAGE + 1, 1).has_value());
        live.decommit_above(0);
        REQUIRE(live.committed() == 4 * PAGE);
    }
}

TEST_CASE("StackCursor laws hold over random operation sequences", "[tiled][stack]")
{
    for (auto seed = 1U; seed <= 50; ++seed) {
        CAPTURE(seed);
        auto rng = std::mt19937 { seed };
        auto cursor = *StackCursor::make(RESERVED, PAGE);
        auto blocks = std::vector<Block> {};

        for (auto step = 0; step < 300; ++step) {
            CAPTURE(step);
            auto const before = cursor;
            switch (rng() % 5) {
            case 0: {
                auto const bytes = std::uint64_t { rng() % (RESERVED / 4) };
                auto const align = std::uint64_t { 1 } << (rng() % 7);
                auto const offset = cursor.push(bytes, align);
                auto const fits = round_up(before.top(), align) + bytes <= RESERVED;
                REQUIRE(offset.has_value() == fits);
                if (offset) {
                    REQUIRE(*offset % align == 0);
                    REQUIRE(*offset >= before.top());
                    REQUIRE(cursor.top() == *offset + bytes);
                    REQUIRE(cursor.committed() == std::max(before.committed(), round_up(cursor.top(), PAGE)));
                    blocks.push_back({ *offset, bytes });
                } else {
                    REQUIRE(unchanged(cursor, before));
                }
                break;
            }
            case 1: {
                if (blocks.empty()) {
                    break;
                }
                auto const top = blocks.back();
                auto const adjacent = top.offset + top.bytes == before.top();
                cursor.pop_top(top.offset, top.bytes);
                REQUIRE(cursor.top() == (adjacent ? top.offset : before.top()));
                REQUIRE(cursor.committed() == before.committed());
                blocks.pop_back();
                break;
            }
            case 2: {
                if (blocks.size() < 2) {
                    break;
                }
                auto const below = blocks[rng() % (blocks.size() - 1)];
                if (below.offset + below.bytes == before.top()) {
                    break;
                }
                cursor.pop_top(below.offset, below.bytes);
                REQUIRE(unchanged(cursor, before));
                REQUIRE_FALSE(cursor.extend_top(below.offset, below.bytes, below.bytes + 1));
                REQUIRE(unchanged(cursor, before));
                break;
            }
            case 3: {
                if (blocks.empty()) {
                    break;
                }
                auto& top = blocks.back();
                auto const new_bytes = std::uint64_t { rng() % (RESERVED / 2) };
                auto const adjacent = top.offset + top.bytes == before.top();
                auto const extended = cursor.extend_top(top.offset, top.bytes, new_bytes);
                REQUIRE(extended == (adjacent && top.offset + new_bytes <= RESERVED));
                if (extended) {
                    REQUIRE(cursor.top() == top.offset + new_bytes);
                    REQUIRE(cursor.committed() == std::max(before.committed(), round_up(cursor.top(), PAGE)));
                    top.bytes = new_bytes;
                } else {
                    REQUIRE(unchanged(cursor, before));
                }
                break;
            }
            default: {
                auto const keep = std::uint64_t { rng() % (RESERVED + PAGE) };
                cursor.decommit_above(keep);
                REQUIRE(cursor.top() == before.top());
                REQUIRE(cursor.committed() == std::min(before.committed(), round_up(std::max(keep, before.top()), PAGE)));
                break;
            }
            }
            REQUIRE(holds_invariant(cursor));
        }
    }
}
