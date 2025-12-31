// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/core/types.hpp"

TEST_CASE("NodeId constants", "[types]")
{
    REQUIRE(pup::INVALID_NODE_ID == 0);
    REQUIRE(pup::SOURCE_ROOT_ID == 0);  // Source root is sentinel value
    REQUIRE(pup::BUILD_ROOT_ID == 1);   // Build root is pre-allocated
}

TEST_CASE("NodeFlags bitwise operations", "[types]")
{
    using enum pup::NodeFlags;

    SECTION("combine flags")
    {
        auto const flags = Modified | Created;
        REQUIRE(pup::has_flag(flags, Modified));
        REQUIRE(pup::has_flag(flags, Created));
        REQUIRE_FALSE(pup::has_flag(flags, Deleted));
    }

    SECTION("check None")
    {
        REQUIRE_FALSE(pup::has_flag(None, Modified));
        REQUIRE_FALSE(pup::has_flag(None, Created));
    }
}

TEST_CASE("NodeType enum values", "[types]")
{
    // Ensure enum values match tup's convention
    REQUIRE(static_cast<int>(pup::NodeType::File) == 0);
    REQUIRE(static_cast<int>(pup::NodeType::Command) == 1);
    REQUIRE(static_cast<int>(pup::NodeType::Directory) == 2);
    REQUIRE(static_cast<int>(pup::NodeType::Variable) == 3);
    REQUIRE(static_cast<int>(pup::NodeType::Generated) == 4);
    REQUIRE(static_cast<int>(pup::NodeType::Ghost) == 5);
    REQUIRE(static_cast<int>(pup::NodeType::Group) == 6);
    REQUIRE(static_cast<int>(pup::NodeType::GeneratedDir) == 7);
}

TEST_CASE("LinkType enum values", "[types]")
{
    REQUIRE(static_cast<int>(pup::LinkType::Normal) == 1);
    REQUIRE(static_cast<int>(pup::LinkType::Sticky) == 2);
    REQUIRE(static_cast<int>(pup::LinkType::Group) == 3);
}
