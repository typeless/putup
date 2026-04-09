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

TEST_CASE("PathPool rooted path algebra", "[path_pool]")
{
    auto pool = PathPool {};
    auto& sp = global_pool();

    SECTION("reserved roots are distinct")
    {
        REQUIRE(PathId::Ungrounded != PathId::SourceRoot);
        REQUIRE(PathId::SourceRoot != PathId::BuildRoot);
        REQUIRE(PathId::Ungrounded != PathId::BuildRoot);
    }

    SECTION("reserved roots are roots")
    {
        REQUIRE(is_root(PathId::Ungrounded));
        REQUIRE(is_root(PathId::SourceRoot));
        REQUIRE(is_root(PathId::BuildRoot));
    }

    SECTION("reserved roots are fixed points of parent")
    {
        REQUIRE(pool.parent(PathId::Ungrounded) == PathId::Ungrounded);
        REQUIRE(pool.parent(PathId::SourceRoot) == PathId::SourceRoot);
        REQUIRE(pool.parent(PathId::BuildRoot) == PathId::BuildRoot);
    }

    SECTION("root() walks to root ancestor")
    {
        auto src = pool.intern_path("gcc/foo.c", sp);
        REQUIRE(pool.root(src) == PathId::Ungrounded);

        auto build = pool.intern_path("gcc/foo.o", sp, PathId::BuildRoot);
        REQUIRE(pool.root(build) == PathId::BuildRoot);

        auto source = pool.intern_path("gcc/foo.c", sp, PathId::SourceRoot);
        REQUIRE(pool.root(source) == PathId::SourceRoot);
    }

    SECTION("is_grounded")
    {
        auto ungrounded = pool.intern_path("gcc/foo.c", sp);
        REQUIRE_FALSE(pool.is_grounded(ungrounded));

        auto grounded = pool.intern_path("gcc/foo.o", sp, PathId::BuildRoot);
        REQUIRE(pool.is_grounded(grounded));
    }

    SECTION("same relative path under different roots produces different PathIds")
    {
        auto source = pool.intern_path("gcc/foo.c", sp, PathId::SourceRoot);
        auto build = pool.intern_path("gcc/foo.c", sp, PathId::BuildRoot);
        auto ungrounded = pool.intern_path("gcc/foo.c", sp);
        REQUIRE(source != build);
        REQUIRE(source != ungrounded);
        REQUIRE(build != ungrounded);
    }

    SECTION("to_string skips root component")
    {
        auto build = pool.intern_path("gcc/libcody/libcody.a", sp, PathId::BuildRoot);
        REQUIRE(sv(pool.to_string(build, sp)) == "gcc/libcody/libcody.a");

        auto source = pool.intern_path("gcc/main.c", sp, PathId::SourceRoot);
        REQUIRE(sv(pool.to_string(source, sp)) == "gcc/main.c");
    }

    SECTION("ground re-interns ungrounded path under root")
    {
        auto ungrounded = pool.intern_path("gcc/libcody/libcody.a", sp);
        REQUIRE(pool.root(ungrounded) == PathId::Ungrounded);

        auto grounded = pool.ground(ungrounded, PathId::BuildRoot);
        REQUIRE(pool.root(grounded) == PathId::BuildRoot);
        REQUIRE(sv(pool.to_string(grounded, sp)) == "gcc/libcody/libcody.a");
    }

    SECTION("ground produces same PathId as direct intern_path with root")
    {
        auto ungrounded = pool.intern_path("gcc/foo.o", sp);
        auto grounded = pool.ground(ungrounded, PathId::BuildRoot);
        auto direct = pool.intern_path("gcc/foo.o", sp, PathId::BuildRoot);
        REQUIRE(grounded == direct);
    }

    SECTION("ground of Ungrounded sentinel returns target root")
    {
        REQUIRE(pool.ground(PathId::Ungrounded, PathId::SourceRoot) == PathId::SourceRoot);
        REQUIRE(pool.ground(PathId::Ungrounded, PathId::BuildRoot) == PathId::BuildRoot);
    }

    SECTION("intern preserves root of parent")
    {
        auto build_gcc = pool.intern(PathId::BuildRoot, intern("gcc"));
        REQUIRE(pool.root(build_gcc) == PathId::BuildRoot);

        auto build_gcc_foo = pool.intern(build_gcc, intern("foo.o"));
        REQUIRE(pool.root(build_gcc_foo) == PathId::BuildRoot);
    }
}
