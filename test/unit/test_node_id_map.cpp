// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/node_id_map.hpp"

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
