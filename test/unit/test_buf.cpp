// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/bump_alloc.hpp"
#include "pup/core/string_pool.hpp"

#include <cstddef>
#include <string_view>

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

TEST_CASE("Buf spill stays off the bump region", "[buf]")
{
    auto buf = Buf {};
    auto const before = pup::bump_allocated_bytes();
    for (int i = 0; i < 8000; ++i) {
        buf += 'x';
    }
    auto const after = pup::bump_allocated_bytes();
    REQUIRE(buf.size() == 8000);
    REQUIRE(after == before);
}

TEST_CASE("Buf spill pointer is stable while the bump top is taken", "[buf]")
{
    auto buf = Buf {};
    for (int i = 0; i < 5000; ++i) {
        buf += 'a';
    }
    auto const* spilled = buf.data();
    (void)pup::bump_alloc(64, 8);
    for (int i = 0; i < 20000; ++i) {
        buf += 'b';
    }
    REQUIRE(buf.data() == spilled);
    REQUIRE(buf.view()[0] == 'a');
    REQUIRE(buf.view()[buf.size() - 1] == 'b');
}

TEST_CASE("Buf grows past the spill reservation off the bump", "[buf]")
{
    auto buf = Buf {};
    auto const chunk = std::string_view { "0123456789abcdef" };
    auto const target = std::size_t { 20 } << 20;
    auto const before = pup::bump_allocated_bytes();
    while (buf.size() < target) {
        buf += chunk;
    }
    auto const after = pup::bump_allocated_bytes();
    REQUIRE(after == before);
    REQUIRE(buf.view().substr(0, chunk.size()) == chunk);
    REQUIRE(buf.view().substr(buf.size() - chunk.size()) == chunk);
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
