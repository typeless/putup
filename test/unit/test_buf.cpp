// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/string_pool.hpp"

using pup::Buf;

TEST_CASE("Buf basic operations", "[buf]")
{
    auto buf = Buf {};

    SECTION("default is empty")
    {
        REQUIRE(buf.empty());
        REQUIRE(buf.size() == 0);
    }

    SECTION("append stays inline")
    {
        buf.append("hello");
        REQUIRE(buf.view() == "hello");
    }

    SECTION("c_str is null-terminated")
    {
        buf.append("test");
        REQUIRE(buf.c_str()[4] == '\0');
    }

    SECTION("operator+=")
    {
        buf += "a";
        buf += 'b';
        buf += "c";
        REQUIRE(buf.view() == "abc");
    }

    SECTION("overflow to heap")
    {
        for (int i = 0; i < 300; ++i) {
            buf += 'x';
        }
        REQUIRE(buf.size() == 300);
        REQUIRE(buf.view().substr(0, 3) == "xxx");
    }

    SECTION("clear resets")
    {
        buf.append("data");
        buf.clear();
        REQUIRE(buf.empty());
    }
}

TEST_CASE("Buf fmt", "[buf]")
{
    auto buf = Buf {};
    buf.fmt("hello {} #{}", "world", 42);
    REQUIRE(buf.view() == "hello world #42");
}

TEST_CASE("Buf fmt escaped braces", "[buf]")
{
    auto buf = Buf {};
    buf.fmt("{{key}}: {}", "value");
    REQUIRE(buf.view() == "{key}: value");
}

TEST_CASE("Buf clear then reuse", "[buf]")
{
    auto buf = Buf {};
    buf.append("old data");
    buf.clear();
    buf.append("new");
    REQUIRE(buf.view() == "new");
}

TEST_CASE("Buf intern", "[buf]")
{
    auto pool = pup::StringPool {};
    auto buf = Buf {};
    buf.append("interned");
    auto id = buf.intern(pool);
    REQUIRE(pool.get(id) == "interned");
}

TEST_CASE("Buf overflow then intern", "[buf]")
{
    auto pool = pup::StringPool {};
    auto buf = Buf {};
    for (int i = 0; i < 300; ++i) {
        buf += 'a';
    }
    auto id = buf.intern(pool);
    auto sv = pool.get(id);
    REQUIRE(sv.size() == 300);
    REQUIRE(sv == buf.view());
}
