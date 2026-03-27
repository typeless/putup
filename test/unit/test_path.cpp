// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"

using namespace pup::path;

namespace {
auto sv(pup::StringId id) -> std::string_view { return pup::global_pool().get(id); }
} // namespace

TEST_CASE("path::join", "[path]")
{
    SECTION("basic join")
    {
        REQUIRE(sv(join("src", "foo.c")) == "src/foo.c");
    }

    SECTION("trailing slash on lhs")
    {
        REQUIRE(sv(join("src/", "foo.c")) == "src/foo.c");
    }

    SECTION("empty lhs")
    {
        REQUIRE(sv(join("", "foo.c")) == "foo.c");
    }

    SECTION("empty rhs")
    {
        REQUIRE(sv(join("src", "")) == "src");
    }

    SECTION("absolute rhs replaces")
    {
        REQUIRE(sv(join("src", "/usr/include")) == "/usr/include");
    }

    SECTION("both empty")
    {
        REQUIRE(sv(join("", "")) == "");
    }
}

TEST_CASE("path::parent", "[path]")
{
    SECTION("nested path")
    {
        REQUIRE(parent("src/lib/foo.c") == "src/lib");
    }

    SECTION("single component")
    {
        REQUIRE(parent("foo.c") == "");
    }

    SECTION("empty")
    {
        REQUIRE(parent("") == "");
    }

    SECTION("root")
    {
        REQUIRE(parent("/") == "/");
    }

    SECTION("root with file")
    {
        REQUIRE(parent("/foo") == "/");
    }

    SECTION("trailing slash")
    {
        REQUIRE(parent("src/lib/") == "src");
    }
}

TEST_CASE("path::filename", "[path]")
{
    SECTION("nested")
    {
        REQUIRE(filename("src/foo.c") == "foo.c");
    }

    SECTION("just filename")
    {
        REQUIRE(filename("foo.c") == "foo.c");
    }

    SECTION("trailing slash")
    {
        REQUIRE(filename("src/") == "");
    }

    SECTION("empty")
    {
        REQUIRE(filename("") == "");
    }
}

TEST_CASE("path::stem", "[path]")
{
    SECTION("basic")
    {
        REQUIRE(stem("foo.c") == "foo");
    }

    SECTION("double extension")
    {
        REQUIRE(stem("foo.tar.gz") == "foo.tar");
    }

    SECTION("no extension")
    {
        REQUIRE(stem("Makefile") == "Makefile");
    }

    SECTION("dotfile")
    {
        REQUIRE(stem(".gitignore") == ".gitignore");
    }

    SECTION("path prefix stripped")
    {
        REQUIRE(stem("src/foo.o") == "foo");
    }
}

TEST_CASE("path::extension", "[path]")
{
    SECTION("basic")
    {
        REQUIRE(extension("foo.c") == ".c");
    }

    SECTION("double extension")
    {
        REQUIRE(extension("foo.tar.gz") == ".gz");
    }

    SECTION("no extension")
    {
        REQUIRE(extension("Makefile") == "");
    }

    SECTION("dotfile")
    {
        REQUIRE(extension(".gitignore") == "");
    }
}

TEST_CASE("path::is_absolute", "[path]")
{
    REQUIRE(is_absolute("/usr/bin"));
    REQUIRE(is_absolute("/"));
    REQUIRE_FALSE(is_absolute("src/foo.c"));
    REQUIRE_FALSE(is_absolute(""));
    REQUIRE_FALSE(is_absolute("../foo"));
}

TEST_CASE("path::normalize", "[path]")
{
    SECTION("dot segments")
    {
        REQUIRE(sv(normalize("src/./foo.c")) == "src/foo.c");
    }

    SECTION("dotdot segments")
    {
        REQUIRE(sv(normalize("src/../include/foo.h")) == "include/foo.h");
    }

    SECTION("complex")
    {
        REQUIRE(sv(normalize("a/b/c/../../d")) == "a/d");
    }

    SECTION("absolute path")
    {
        REQUIRE(sv(normalize("/a/b/../c")) == "/a/c");
    }

    SECTION("absolute excess dotdot absorbed")
    {
        REQUIRE(sv(normalize("/a/../..")) == "/");
    }

    SECTION("absolute single dotdot")
    {
        REQUIRE(sv(normalize("/..")) == "/");
    }

    SECTION("relative excess dotdot preserved")
    {
        REQUIRE(sv(normalize("a/../../b")) == "../b");
    }

    SECTION("empty normalizes to dot")
    {
        REQUIRE(sv(normalize("")) == ".");
    }

    SECTION("just dot")
    {
        REQUIRE(sv(normalize(".")) == ".");
    }

    SECTION("double slashes")
    {
        REQUIRE(sv(normalize("src//foo.c")) == "src/foo.c");
    }

    SECTION("trailing slash")
    {
        REQUIRE(sv(normalize("src/lib/")) == "src/lib");
    }
}

TEST_CASE("path::relative", "[path]")
{
    SECTION("child of base")
    {
        REQUIRE(sv(relative("a/b/c", "a")) == "b/c");
    }

    SECTION("same path")
    {
        REQUIRE(sv(relative("a/b", "a/b")) == ".");
    }

    SECTION("sibling")
    {
        REQUIRE(sv(relative("x/y", "a/b")) == "../../x/y");
    }

    SECTION("base is child")
    {
        REQUIRE(sv(relative("a", "a/b/c")) == "../..");
    }

    SECTION("both empty")
    {
        REQUIRE(sv(relative("", "")) == ".");
    }
}
