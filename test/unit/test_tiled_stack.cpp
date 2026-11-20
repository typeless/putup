// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "catch_amalgamated.hpp"
#include "tiled/layout.hpp"
#include "tiled/platform.hpp"
#include "tiled/space_storage.hpp"
#include "tiled/stack.hpp"
#include "tiled/stack_slot.hpp"
#include "tiled/tile.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

using tiled::Stack;
using tiled::StackSlot;
using tiled::Tile;

namespace {

constexpr auto stack_layout = tiled::Layout {
    .root_log2 = 47,
    .image_nodes = { { 0, 33 }, { std::uint64_t { 1 } << 46, 46 } },
    .base = std::uint64_t { 0x3100'0000'0000 },
    .pool_log2 = 30,
    .heap_log2 = 26,
    .min_tile_log2 = 24,
};

constinit tiled::SpaceStorage<stack_layout> stack_space; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

constexpr auto TILE_LOG2 = std::uint8_t { 24 };
constexpr auto TILE_BYTES = std::size_t { 1 } << TILE_LOG2;

auto fresh_stack() -> Stack
{
    return Stack { stack_space.pool(), TILE_LOG2 };
}

static_assert(!std::is_copy_constructible_v<Tile>);
static_assert(!std::is_move_constructible_v<Tile>);
static_assert(!std::is_copy_constructible_v<Stack>);
static_assert(!std::is_move_constructible_v<Stack>);
static_assert(!std::is_copy_constructible_v<StackSlot>);
static_assert(!std::is_copy_assignable_v<StackSlot>);
static_assert(std::is_nothrow_move_constructible_v<StackSlot>);
static_assert(std::is_nothrow_move_assignable_v<StackSlot>);

auto round_up_to_page(std::size_t n) -> std::size_t
{
    auto const page = tiled::page_size();
    return (n + page - 1) / page * page;
}

} // namespace

TEST_CASE("Stack starts empty over the whole tile", "[tiled][stack]")
{
    auto const stack = fresh_stack();
    REQUIRE(stack.top() == 0);
    REQUIRE(stack.committed() == 0);
    REQUIRE(stack.reserved() == TILE_BYTES);
    REQUIRE(reinterpret_cast<std::uintptr_t>(stack.base()) % TILE_BYTES == 0);
}

TEST_CASE("Stack push returns usable committed memory at the cursor's offset", "[tiled][stack]")
{
    auto stack = fresh_stack();
    auto const page = tiled::page_size();

    auto* const first = static_cast<std::byte*>(stack.push(10, 8));
    REQUIRE(first == stack.base());
    auto* const second = static_cast<std::byte*>(stack.push(page, 64));
    REQUIRE(second == static_cast<std::byte*>(stack.base()) + 64);
    REQUIRE(stack.top() == 64 + page);
    REQUIRE(stack.committed() == round_up_to_page(64 + page));

    std::memset(first, 0x11, 10);
    std::memset(second, 0x22, page);
    REQUIRE(second[page - 1] == std::byte { 0x22 });
}

TEST_CASE("Stack extend_top and pop_top move only the top block", "[tiled][stack]")
{
    auto stack = fresh_stack();
    auto* const below = stack.push(100, 8);
    auto* const top = stack.push(100, 8);

    REQUIRE_FALSE(stack.extend_top(below, 100, 200));
    REQUIRE(static_cast<std::byte*>(top) == static_cast<std::byte*>(below) + 104);
    stack.pop_top(below, 100);
    REQUIRE(stack.top() == 204);

    auto const page = tiled::page_size();
    REQUIRE(stack.extend_top(top, 100, 3 * page));
    REQUIRE(stack.committed() == round_up_to_page(104 + 3 * page));
    static_cast<std::byte*>(top)[3 * page - 1] = std::byte { 0x33 };

    REQUIRE_FALSE(stack.extend_top(top, 3 * page, TILE_BYTES));
    stack.pop_top(top, 3 * page);
    REQUIRE(stack.top() == 104);
}

TEST_CASE("Stack refuses extend_top and ignores pop_top for a pointer outside its tile", "[tiled][stack]")
{
    auto stack = fresh_stack();
    auto* const block = static_cast<std::byte*>(stack.push(100, 8));
    auto* const below = block - 8;
    auto* const above = block + TILE_BYTES;

    REQUIRE_FALSE(stack.extend_top(below, 108, 200));
    REQUIRE_FALSE(stack.extend_top(above, 0, 8));
    stack.pop_top(below, 108);
    stack.pop_top(above, 0);
    REQUIRE(stack.top() == 100);
}

TEST_CASE("Stack decommit_above returns pages that read as zero when committed again", "[tiled][stack]")
{
    auto stack = fresh_stack();
    auto const page = tiled::page_size();
    auto* const block = static_cast<std::byte*>(stack.push(4 * page, 8));
    std::memset(block, 0x44, 4 * page);
    stack.pop_top(block, 4 * page);

    stack.decommit_above(page);
    REQUIRE(stack.committed() == page);

    auto* const again = static_cast<std::byte*>(stack.push(4 * page, 8));
    REQUIRE(again == block);
    REQUIRE(again[0] == std::byte { 0x44 });
    REQUIRE(again[page] == std::byte { 0 });
    REQUIRE(again[4 * page - 1] == std::byte { 0 });
}

TEST_CASE("Stack returns its tile when destroyed", "[tiled][stack]")
{
    void* base = nullptr;
    {
        auto const stack = fresh_stack();
        base = stack.base();
    }
    auto const again = fresh_stack();
    REQUIRE(again.base() == base);
}

TEST_CASE("StackSlot is empty until a stack is emplaced", "[tiled][stack]")
{
    auto slot = StackSlot {};
    REQUIRE_FALSE(slot);
    auto const& stack = slot.emplace(stack_space.pool(), TILE_LOG2);
    REQUIRE(slot);
    REQUIRE(slot->base() == stack.base());
    REQUIRE(slot->reserved() == TILE_BYTES);
    slot.reset();
    REQUIRE_FALSE(slot);
}

TEST_CASE("StackSlot returns its tile exactly once", "[tiled][stack]")
{
    void* base = nullptr;

    SECTION("when it is reset")
    {
        auto slot = StackSlot {};
        base = slot.emplace(stack_space.pool(), TILE_LOG2).base();
        slot.reset();
        slot.reset();
    }
    SECTION("when it is destroyed")
    {
        auto slot = StackSlot {};
        base = slot.emplace(stack_space.pool(), TILE_LOG2).base();
    }
    SECTION("when it is emplaced a second time")
    {
        auto slot = StackSlot {};
        base = slot.emplace(stack_space.pool(), TILE_LOG2).base();
        REQUIRE(slot.emplace(stack_space.pool(), TILE_LOG2).base() == base);
    }
    SECTION("when its stack moved to another slot and both are destroyed")
    {
        auto source = StackSlot {};
        base = source.emplace(stack_space.pool(), TILE_LOG2).base();
        auto const target = StackSlot { std::move(source) };
        REQUIRE(target);
    }

    auto const again = fresh_stack();
    REQUIRE(again.base() == base);
}

TEST_CASE("StackSlot move construction transfers the stack and empties the source", "[tiled][stack]")
{
    auto source = StackSlot {};
    auto* const block = static_cast<std::byte*>(source.emplace(stack_space.pool(), TILE_LOG2).push(100, 1));
    block[99] = std::byte { 0x5A };
    auto const committed = source->committed();

    auto target = StackSlot { std::move(source) };

    REQUIRE_FALSE(source); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    REQUIRE(target);
    REQUIRE(target->base() == block);
    REQUIRE(target->top() == 100);
    REQUIRE(target->committed() == committed);
    REQUIRE(block[99] == std::byte { 0x5A });
    REQUIRE(target->extend_top(block, 100, 200));
}

TEST_CASE("StackSlot move assignment", "[tiled][stack]")
{
    auto source = StackSlot {};
    auto target = StackSlot {};
    auto* const source_base = source.emplace(stack_space.pool(), TILE_LOG2).base();

    SECTION("into an empty slot transfers the stack")
    {
        target = std::move(source);
        REQUIRE_FALSE(source); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        REQUIRE(target->base() == source_base);
    }
    SECTION("over a held stack returns that stack's tile first")
    {
        auto* const target_base = target.emplace(stack_space.pool(), TILE_LOG2).base();
        REQUIRE(target_base != source_base);
        target = std::move(source);
        REQUIRE_FALSE(source); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        REQUIRE(target->base() == source_base);
        auto const again = fresh_stack();
        REQUIRE(again.base() == target_base);
    }
    SECTION("from an empty slot returns the held tile")
    {
        source = std::move(target);
        REQUIRE_FALSE(source);
        auto const again = fresh_stack();
        REQUIRE(again.base() == source_base);
    }
    SECTION("to itself keeps the stack")
    {
        auto& alias = source;
        source = std::move(alias);
        REQUIRE(source);
        REQUIRE(source->base() == source_base);
    }
}
