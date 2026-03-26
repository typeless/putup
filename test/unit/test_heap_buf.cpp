// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/heap_buf.hpp"
#include "pup/core/string_pool.hpp"

using pup::HeapBuf;

TEST_CASE("HeapBuf basic operations", "[heap_buf]")
{
    auto buf = HeapBuf {};

    SECTION("default is empty")
    {
        REQUIRE(buf.empty());
        REQUIRE(buf.size() == 0);
    }

    SECTION("append and view")
    {
        buf.append("hello");
        buf += ' ';
        buf += "world";
        REQUIRE(buf.view() == "hello world");
        REQUIRE(buf.size() == 11);
    }

    SECTION("c_str is null-terminated")
    {
        buf.append("test");
        REQUIRE(buf.c_str()[4] == '\0');
    }

    SECTION("clear resets")
    {
        buf.append("data");
        buf.clear();
        REQUIRE(buf.empty());
    }

    SECTION("reserve pre-allocates")
    {
        buf.reserve(1000);
        buf.append("after reserve");
        REQUIRE(buf.view() == "after reserve");
    }

    SECTION("resize and mutable data")
    {
        buf.resize(5);
        REQUIRE(buf.size() == 5);
        auto* p = buf.data();
        p[0] = 'h';
        p[1] = 'i';
        p[2] = '\0';
    }

    SECTION("large append triggers growth")
    {
        for (int i = 0; i < 1000; ++i) {
            buf += 'x';
        }
        REQUIRE(buf.size() == 1000);
    }
}

TEST_CASE("HeapBuf fmt", "[heap_buf]")
{
    auto buf = HeapBuf {};
    buf.fmt("error: {} at line {}", "syntax", 10);
    REQUIRE(buf.view() == "error: syntax at line 10");
}

// HeapBuf is non-movable (YAGNI — no function returns it yet)

TEST_CASE("HeapBuf fmt escaped braces", "[heap_buf]")
{
    auto buf = HeapBuf {};
    buf.fmt("{{key}}: {}", "value");
    REQUIRE(buf.view() == "{key}: value");
}

TEST_CASE("HeapBuf clear then reuse", "[heap_buf]")
{
    auto buf = HeapBuf {};
    buf.append("old data");
    buf.clear();
    buf.append("new");
    REQUIRE(buf.view() == "new");
}

TEST_CASE("HeapBuf intern", "[heap_buf]")
{
    auto pool = pup::StringPool {};
    auto buf = HeapBuf {};
    buf.append("interned");
    auto id = buf.intern(pool);
    REQUIRE(pool.get(id) == "interned");
}
