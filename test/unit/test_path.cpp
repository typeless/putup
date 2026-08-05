// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

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

TEST_CASE("path::is_root", "[path]")
{
    REQUIRE(is_root("/"));
    REQUIRE_FALSE(is_root("/a"));
    REQUIRE_FALSE(is_root(""));
    REQUIRE_FALSE(is_root("src"));
#ifdef _WIN32
    REQUIRE(is_root("C:/"));
    REQUIRE(is_root("C:\\"));
    REQUIRE_FALSE(is_root("C:/a"));
    REQUIRE_FALSE(is_root("C:"));
#endif
}

// The precondition the textual predicates assert. It has to accept everything normalize() emits,
// or the assertions fire on paths that are already in hand normalized (#316).
TEST_CASE("path::is_normal", "[path]")
{
    SECTION("what normalize would return")
    {
        REQUIRE(is_normal(""));
        REQUIRE(is_normal("."));
        REQUIRE(is_normal("/"));
        REQUIRE(is_normal("a"));
        REQUIRE(is_normal("a/b"));
        REQUIRE(is_normal("/a/b"));
        REQUIRE(is_normal(".."));
        REQUIRE(is_normal("../../a"));
    }

    SECTION("what it would collapse")
    {
        REQUIRE_FALSE(is_normal("./a"));
        REQUIRE_FALSE(is_normal("a/./b"));
        REQUIRE_FALSE(is_normal("a/../b"));
        REQUIRE_FALSE(is_normal("a//b"));
        REQUIRE_FALSE(is_normal("a/"));
        REQUIRE_FALSE(is_normal("/a/"));
        REQUIRE_FALSE(is_normal("/.."));
    }

    SECTION("agrees with normalize on the paths normalize builds")
    {
        auto const inputs = std::array<std::string_view, 8> {
            "a/./b", "a/../../b", "/a/b/../c", "src//foo.c", "src/lib/", "..", "/..", ""
        };
        for (auto in : inputs) {
            INFO("input: " << in);
            REQUIRE(is_normal(sv(normalize(in))));
        }
    }
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

#ifdef _WIN32

TEST_CASE("path::normalize keeps the drive root", "[path][windows]")
{
    SECTION("drive-absolute path is unchanged")
    {
        REQUIRE(sv(normalize("C:/a/b")) == "C:/a/b");
    }

    SECTION("dotdot resolves under the drive")
    {
        REQUIRE(sv(normalize("C:/a/../b")) == "C:/b");
    }

    SECTION("dotdot cannot escape the drive")
    {
        REQUIRE(sv(normalize("C:/..")) == "C:/");
        REQUIRE(sv(normalize("C:/a/../..")) == "C:/");
    }

    SECTION("drive root normalizes to itself")
    {
        REQUIRE(sv(normalize("C:/")) == "C:/");
    }

    SECTION("trailing slash dropped below the root")
    {
        REQUIRE(sv(normalize("C:/a/")) == "C:/a");
    }

    SECTION("dot segments and double slashes")
    {
        REQUIRE(sv(normalize("C:/./a//b")) == "C:/a/b");
    }

    SECTION("drive letter case preserved")
    {
        REQUIRE(sv(normalize("c:/a")) == "c:/a");
    }

    SECTION("backslash root emits as forward slash")
    {
        REQUIRE(sv(normalize("C:\\a")) == "C:/a");
    }
}

TEST_CASE("path::is_absolute on drive-absolute paths", "[path][windows]")
{
    REQUIRE(is_absolute("C:/a"));
    REQUIRE(is_absolute("C:\\a"));
    REQUIRE_FALSE(is_absolute("C:"));
    REQUIRE_FALSE(is_absolute("C:a"));
}

TEST_CASE("path::parent on drive-absolute paths", "[path][windows]")
{
    REQUIRE(parent("C:/a/b") == "C:/a");
    REQUIRE(parent("C:/a") == "C:/");
    REQUIRE(parent("C:/") == "C:/");
}

TEST_CASE("path::filename on drive-absolute paths", "[path][windows]")
{
    REQUIRE(filename("C:/a") == "a");
    REQUIRE(filename("C:/") == "");
}

TEST_CASE("path::join on drive-absolute paths", "[path][windows]")
{
    REQUIRE(sv(join("C:/a", "b")) == "C:/a/b");
    REQUIRE(sv(join("x", "C:/b")) == "C:/b");
}

TEST_CASE("path::relative on drive-absolute paths", "[path][windows]")
{
    REQUIRE(sv(relative("C:/a/b", "C:/a")) == "b");
    REQUIRE(sv(relative("C:/a", "C:/a")) == ".");
}

#endif

namespace {

auto check_normalize_laws(std::string const& p, std::string_view root) -> void
{
    auto const once = std::string { sv(normalize(p)) };
    auto const twice = std::string { sv(normalize(once)) };
    INFO("input: " << p << "\nnormalize: " << once << "\nagain: " << twice);

    REQUIRE(once == twice);

    auto const absolute = is_absolute(p);
    if (absolute) {
        REQUIRE(is_absolute(once));
        REQUIRE(std::string_view { once }.starts_with(root));
    }

    if (once == ".") {
        return;
    }

    auto body = std::string_view { once };
    if (absolute) {
        body.remove_prefix(root.size());
    }
    auto seen_name = false;
    auto start = std::size_t { 0 };
    while (start < body.size()) {
        auto end = body.find('/', start);
        if (end == std::string_view::npos) {
            end = body.size();
        }
        auto part = body.substr(start, end - start);
        if (!part.empty()) {
            REQUIRE(part != ".");
            if (part == "..") {
                REQUIRE_FALSE(absolute);
                REQUIRE_FALSE(seen_name);
            } else {
                seen_name = true;
            }
        }
        start = end + 1;
    }
}

auto check_laws_under_root(std::string_view root) -> void
{
    auto const alphabet = std::array<std::string_view, 5> { "a", "b", ".", "..", "" };
    for (auto x : alphabet) {
        check_normalize_laws(std::string { root } + std::string { x }, root);
        for (auto y : alphabet) {
            auto const two = std::string { root } + std::string { x } + "/" + std::string { y };
            check_normalize_laws(two, root);
            for (auto z : alphabet) {
                check_normalize_laws(two + "/" + std::string { z }, root);
            }
        }
    }
}

} // namespace

TEST_CASE("path::normalize obeys its laws", "[path]")
{
    SECTION("relative paths")
    {
        check_laws_under_root("");
    }

    SECTION("rooted paths")
    {
        check_laws_under_root("/");
    }

#ifdef _WIN32
    SECTION("drive-rooted paths")
    {
        check_laws_under_root("C:/");
    }
#endif
}
