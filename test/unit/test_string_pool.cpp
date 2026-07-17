// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdio>

TEST_CASE("StringPool Robin Hood index", "[string_pool]")
{
    auto pool = pup::StringPool {};

    SECTION("basic intern and get")
    {
        auto id = pool.intern("hello");
        REQUIRE(pool.get(id) == "hello");
    }

    SECTION("deduplication")
    {
        auto id1 = pool.intern("same");
        auto id2 = pool.intern("same");
        REQUIRE(id1 == id2);
        REQUIRE(pool.size() == 1);
    }

    SECTION("find existing")
    {
        (void)pool.intern("exists");
        REQUIRE(pool.find("exists") != pup::StringId::Empty);
        REQUIRE(pool.find("missing") == pup::StringId::Empty);
    }

    SECTION("empty string")
    {
        auto id = pool.intern("");
        REQUIRE(id == pup::StringId::Empty);
        REQUIRE(pool.get(pup::StringId::Empty) == "");
    }

    SECTION("interned strings are null-terminated")
    {
        // env callers pass pool string data() straight to setenv/getenv.
        auto a = pool.intern("first");
        auto b = pool.intern("second-longer");
        auto sv_a = pool.get(a);
        auto sv_b = pool.get(b);
        REQUIRE(sv_a.data()[sv_a.size()] == '\0'); // NOLINT(readability-simplify-subscript-expr) — reads past the view on purpose
        REQUIRE(sv_b.data()[sv_b.size()] == '\0'); // NOLINT(readability-simplify-subscript-expr)
    }

    SECTION("bytes reports total interned content")
    {
        (void)pool.intern("abc");
        (void)pool.intern("defgh");
        REQUIRE(pool.bytes() == 8);
    }

    SECTION("stress: 10K unique strings")
    {
        for (auto i = 0; i < 10000; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "var_%d", i);
            auto id = pool.intern(buf);
            REQUIRE(pool.get(id) == buf);
        }
        REQUIRE(pool.size() == 10000);

        for (auto i = 0; i < 10000; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "var_%d", i);
            REQUIRE(pool.find(buf) != pup::StringId::Empty);
        }
    }

    SECTION("stress: 10K duplicate interns")
    {
        (void)pool.intern("repeated");
        for (auto i = 0; i < 10000; ++i) {
            (void)pool.intern("repeated");
        }
        REQUIRE(pool.size() == 1);
    }

    SECTION("reserve pre-allocates")
    {
        pool.reserve(1000);
        for (auto i = 0; i < 1000; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "r_%d", i);
            (void)pool.intern(buf);
        }
        REQUIRE(pool.size() == 1000);
    }

    SECTION("clear resets everything")
    {
        (void)pool.intern("a");
        (void)pool.intern("b");
        pool.clear();
        REQUIRE(pool.size() == 0);
        REQUIRE(pool.find("a") == pup::StringId::Empty);
    }

    SECTION("move semantics")
    {
        (void)pool.intern("before_move");
        auto id = pool.find("before_move");

        auto moved = std::move(pool);
        REQUIRE(moved.get(id) == "before_move");
        REQUIRE(moved.find("before_move") == id);
    }
}
