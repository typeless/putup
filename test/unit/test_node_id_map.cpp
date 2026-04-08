// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/arena.hpp"
#include "pup/core/node_id_map.hpp"

#include <algorithm>
#include <vector>

using namespace pup;

TEST_CASE("NodeIdMap32 dispatches by node type", "[node_id_map]")
{
    auto map = NodeIdMap32 {};
    map.resize_files(100);
    map.resize_commands(50);
    map.resize_conditions(10);
    map.resize_phis(10);

    auto file_id = NodeId { 5 };
    auto cmd_id = node_id::make_command(3);
    auto cond_id = node_id::make_condition(2);
    auto phi_id = node_id::make_phi(1);

    SECTION("slots are independent per type")
    {
        map.set(file_id, 111);
        map.set(cmd_id, 222);
        map.set(cond_id, 333);
        map.set(phi_id, 444);
        REQUIRE(map.get(file_id) == 111);
        REQUIRE(map.get(cmd_id) == 222);
        REQUIRE(map.get(cond_id) == 333);
        REQUIRE(map.get(phi_id) == 444);
    }

    SECTION("contains checks correct sub-array")
    {
        map.set(file_id, 1);
        REQUIRE(map.contains(file_id));
        REQUIRE_FALSE(map.contains(cmd_id));
        REQUIRE_FALSE(map.contains(cond_id));
        REQUIRE_FALSE(map.contains(phi_id));
    }

    SECTION("same index different types are independent")
    {
        auto f3 = NodeId { 3 };
        auto c3 = node_id::make_command(3);
        map.set(f3, 10);
        map.set(c3, 20);
        REQUIRE(map.get(f3) == 10);
        REQUIRE(map.get(c3) == 20);
    }

    SECTION("clear resets all sub-arrays")
    {
        map.set(file_id, 1);
        map.set(cmd_id, 2);
        map.clear();
        REQUIRE_FALSE(map.contains(file_id));
        REQUIRE_FALSE(map.contains(cmd_id));
    }
}

TEST_CASE("NodeIdArenaIndex stores and retrieves slices", "[node_id_map]")
{
    auto arena = Arena32 {};
    auto index = NodeIdArenaIndex {};
    index.resize_files(10);
    index.resize_commands(10);

    auto file_id = NodeId { 3 };
    auto cmd_id = node_id::make_command(5);

    SECTION("absent node returns {0, 0}")
    {
        REQUIRE_FALSE(index.contains(file_id));
        auto s = index.get_slice(file_id);
        REQUIRE(s.offset == 0);
        REQUIRE(s.length == 0);
    }

    SECTION("incremental append_extend builds slice")
    {
        auto s = index.get_slice(file_id);
        s = arena.append_extend(s, 100);
        index.set_slice(file_id, s);
        s = arena.append_extend(s, 200);
        index.set_slice(file_id, s);
        s = arena.append_extend(s, 300);
        index.set_slice(file_id, s);

        REQUIRE(index.contains(file_id));
        auto result = index.get_slice(file_id);
        REQUIRE(result.length == 3);

        auto span = arena.slice(result);
        REQUIRE(span[0] == 100);
        REQUIRE(span[1] == 200);
        REQUIRE(span[2] == 300);
    }

    SECTION("different node types are independent")
    {
        auto sf = arena.append_extend(index.get_slice(file_id), 10);
        index.set_slice(file_id, sf);

        auto sc = arena.append_extend(index.get_slice(cmd_id), 20);
        index.set_slice(cmd_id, sc);

        REQUIRE(arena.slice(index.get_slice(file_id))[0] == 10);
        REQUIRE(arena.slice(index.get_slice(cmd_id))[0] == 20);
    }

    SECTION("clear resets all")
    {
        auto s = arena.append_extend(index.get_slice(file_id), 42);
        index.set_slice(file_id, s);
        index.clear();
        REQUIRE_FALSE(index.contains(file_id));
    }
}

TEST_CASE("NodeIdMap32::for_each_id visits all node types", "[node_id_map]")
{
    auto map = NodeIdMap32 {};
    map.resize_files(100);
    map.resize_commands(50);
    map.resize_conditions(10);
    map.resize_phis(10);

    auto file_a = NodeId { 5 };
    auto file_b = NodeId { 42 };
    auto cmd_a = node_id::make_command(3);
    auto cmd_b = node_id::make_command(17);
    auto cond = node_id::make_condition(2);
    auto phi = node_id::make_phi(7);

    map.set(file_a, 1);
    map.set(file_b, 1);
    map.set(cmd_a, 1);
    map.set(cmd_b, 1);
    map.set(cond, 1);
    map.set(phi, 1);

    auto collected = std::vector<NodeId> {};
    map.for_each_id(
        [](NodeId id, void* ctx) {
            static_cast<std::vector<NodeId>*>(ctx)->push_back(id);
        },
        &collected
    );

    REQUIRE(collected.size() == 6);
    std::sort(collected.begin(), collected.end());

    // File IDs have no flag bits, so they sort lowest
    REQUIRE(std::find(collected.begin(), collected.end(), file_a) != collected.end());
    REQUIRE(std::find(collected.begin(), collected.end(), file_b) != collected.end());
    REQUIRE(std::find(collected.begin(), collected.end(), cmd_a) != collected.end());
    REQUIRE(std::find(collected.begin(), collected.end(), cmd_b) != collected.end());
    REQUIRE(std::find(collected.begin(), collected.end(), cond) != collected.end());
    REQUIRE(std::find(collected.begin(), collected.end(), phi) != collected.end());
}

TEST_CASE("NodeIdMap32::for_each_id on empty map visits nothing", "[node_id_map]")
{
    auto map = NodeIdMap32 {};
    auto count = std::size_t { 0 };
    map.for_each_id(
        [](NodeId, void* ctx) {
            ++*static_cast<std::size_t*>(ctx);
        },
        &count
    );
    REQUIRE(count == 0);
}
