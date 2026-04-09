// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/path_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"

using namespace pup;

namespace {

auto intern(std::string_view s) -> StringId
{
    return global_pool().intern(s);
}

auto sv(StringId id) -> std::string_view
{
    return global_pool().get(id);
}

} // namespace

TEST_CASE("PathPool intern and decompose", "[path_pool]")
{
    auto pool = PathPool {};

    SECTION("intern returns non-root PathId")
    {
        auto id = pool.intern(PathId::Root, intern("src"));
        REQUIRE_FALSE(is_root(id));
    }

    SECTION("same args return same PathId (dedup)")
    {
        auto a = pool.intern(PathId::Root, intern("src"));
        auto b = pool.intern(PathId::Root, intern("src"));
        REQUIRE(a == b);
    }

    SECTION("different names return different PathIds")
    {
        auto a = pool.intern(PathId::Root, intern("src"));
        auto b = pool.intern(PathId::Root, intern("lib"));
        REQUIRE(a != b);
    }

    SECTION("parent and name decompose correctly")
    {
        auto src = pool.intern(PathId::Root, intern("src"));
        REQUIRE(pool.parent(src) == PathId::Root);
        REQUIRE(pool.name(src) == intern("src"));
    }

    SECTION("chained paths decompose correctly")
    {
        auto src = pool.intern(PathId::Root, intern("src"));
        auto lib = pool.intern(src, intern("lib"));
        auto foo = pool.intern(lib, intern("foo.c"));

        REQUIRE(pool.parent(foo) == lib);
        REQUIRE(pool.name(foo) == intern("foo.c"));
        REQUIRE(pool.parent(lib) == src);
        REQUIRE(pool.name(lib) == intern("lib"));
        REQUIRE(pool.parent(src) == PathId::Root);
    }

    SECTION("empty name returns parent unchanged")
    {
        auto src = pool.intern(PathId::Root, intern("src"));
        auto same = pool.intern(src, StringId::Empty);
        REQUIRE(same == src);
    }

    SECTION("size counts interned entries")
    {
        REQUIRE(pool.size() == 0);
        (void)pool.intern(PathId::Root, intern("a"));
        REQUIRE(pool.size() == 1);
        (void)pool.intern(PathId::Root, intern("a"));
        REQUIRE(pool.size() == 1);
        (void)pool.intern(PathId::Root, intern("b"));
        REQUIRE(pool.size() == 2);
    }
}

TEST_CASE("PathPool intern_path parses slash-separated paths", "[path_pool]")
{
    auto pool = PathPool {};
    auto& sp = global_pool();

    SECTION("single component")
    {
        auto id = pool.intern_path("src", sp);
        REQUIRE_FALSE(is_root(id));
        REQUIRE(pool.parent(id) == PathId::Root);
        REQUIRE(pool.name(id) == intern("src"));
    }

    SECTION("multi-component path")
    {
        auto id = pool.intern_path("src/lib/foo.c", sp);
        REQUIRE(pool.name(id) == intern("foo.c"));

        auto lib = pool.parent(id);
        REQUIRE(pool.name(lib) == intern("lib"));

        auto src = pool.parent(lib);
        REQUIRE(pool.name(src) == intern("src"));
        REQUIRE(pool.parent(src) == PathId::Root);
    }

    SECTION("structural equality with manual intern")
    {
        auto manual = pool.intern(
            pool.intern(
                pool.intern(PathId::Root, intern("src")),
                intern("lib")),
            intern("foo.c"));
        auto parsed = pool.intern_path("src/lib/foo.c", sp);
        REQUIRE(manual == parsed);
    }

    SECTION("empty path returns Root")
    {
        REQUIRE(pool.intern_path("", sp) == PathId::Root);
    }

    SECTION("dot path returns Root")
    {
        REQUIRE(pool.intern_path(".", sp) == PathId::Root);
    }

    SECTION("path with trailing slash")
    {
        auto a = pool.intern_path("src/", sp);
        auto b = pool.intern_path("src", sp);
        REQUIRE(a == b);
    }

    SECTION("path with dot component skipped")
    {
        auto a = pool.intern_path("src/./lib", sp);
        auto b = pool.intern_path("src/lib", sp);
        REQUIRE(a == b);
    }

    SECTION("dotdot is interned as literal component")
    {
        auto id = pool.intern_path("src/../lib", sp);
        REQUIRE(pool.name(id) == intern("lib"));

        auto dotdot = pool.parent(id);
        REQUIRE(pool.name(dotdot) == intern(".."));

        auto src = pool.parent(dotdot);
        REQUIRE(pool.name(src) == intern("src"));
    }
}

TEST_CASE("PathPool to_string materializes path", "[path_pool]")
{
    auto pool = PathPool {};
    auto& sp = global_pool();

    SECTION("root materializes to empty string")
    {
        auto s = pool.to_string(PathId::Root, sp);
        REQUIRE(is_empty(s));
    }

    SECTION("single component")
    {
        auto id = pool.intern_path("src", sp);
        REQUIRE(sv(pool.to_string(id, sp)) == "src");
    }

    SECTION("multi-component path")
    {
        auto id = pool.intern_path("src/lib/foo.c", sp);
        REQUIRE(sv(pool.to_string(id, sp)) == "src/lib/foo.c");
    }

    SECTION("round-trip: intern_path then to_string")
    {
        auto paths = { "a", "a/b", "a/b/c/d/e.txt", "Makefile" };
        for (auto p : paths) {
            auto id = pool.intern_path(p, sp);
            REQUIRE(sv(pool.to_string(id, sp)) == p);
        }
    }
}

TEST_CASE("PathPool clear resets state", "[path_pool]")
{
    auto pool = PathPool {};
    auto& sp = global_pool();
    (void)pool.intern_path("src/lib/foo.c", sp);
    REQUIRE(pool.size() == 3);

    pool.clear();
    REQUIRE(pool.size() == 0);

    auto id = pool.intern_path("src", sp);
    REQUIRE_FALSE(is_root(id));
    REQUIRE(pool.size() == 1);
}
