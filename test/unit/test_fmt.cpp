// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/fmt.hpp"

using pup::String;
using pup::fmt;

TEST_CASE("fmt basic substitution", "[fmt]")
{
    REQUIRE(fmt("hello {}!", "world") == "hello world!");
    REQUIRE(fmt("{} + {} = {}", 1, 2, 3) == "1 + 2 = 3");
    REQUIRE(fmt("no placeholders") == "no placeholders");
    REQUIRE(fmt("{}", "only") == "only");
}

TEST_CASE("fmt with pup::String args", "[fmt]")
{
    auto s = String { "putup" };
    REQUIRE(fmt("building {}", s) == "building putup");
}

TEST_CASE("fmt char argument", "[fmt]")
{
    REQUIRE(fmt("Expected '(' after '{}'", '!') == "Expected '(' after '!'");
}

TEST_CASE("fmt negative int", "[fmt]")
{
    REQUIRE(fmt("exit code: {}", -1) == "exit code: -1");
}

TEST_CASE("fmt escaped braces", "[fmt]")
{
    REQUIRE(fmt("use {{}} for placeholders") == "use {} for placeholders");
}

TEST_CASE("fmt empty pattern", "[fmt]")
{
    REQUIRE(fmt("") == "");
}

TEST_CASE("fmt adjacent placeholders", "[fmt]")
{
    REQUIRE(fmt("{}{}", "a", "b") == "ab");
}

TEST_CASE("fmt escaped and placeholder mixed", "[fmt]")
{
    REQUIRE(fmt("{{{}}}",  "x") == "{x}");
}
