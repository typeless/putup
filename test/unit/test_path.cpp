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
    // "C:" is the whole prefix a drive-relative path is rooted on, so it is one (#388).
    REQUIRE(is_root("C:"));
    REQUIRE(is_root("//"));
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

/// A drive prefix is the one root that contains no separator byte, so the splitters cannot honour
/// it incidentally the way `find_last_of` honours "/", "C:/" and "//". Each function answers for
/// itself, and on POSIX "C:a" is an ordinary relative name with a colon in it (#388).
TEST_CASE("path::filename answers a separator-less root", "[path]")
{
#ifdef _WIN32
    REQUIRE(filename("C:a") == "a");
    REQUIRE(filename("C:") == "");
#else
    REQUIRE(filename("C:a") == "C:a");
    REQUIRE(filename("C:") == "C:");
#endif
}

TEST_CASE("path::parent answers a separator-less root", "[path]")
{
#ifdef _WIN32
    REQUIRE(parent("C:a") == "C:");
#else
    REQUIRE(parent("C:a") == "");
#endif
}

TEST_CASE("path::join answers a separator-less root", "[path]")
{
#ifdef _WIN32
    REQUIRE(sv(join("C:", "a")) == "C:a");
#else
    REQUIRE(sv(join("C:", "a")) == "C:/a");
#endif
}

/// stem and extension read the name through filename, so the clamp reaches them without their
/// own edit -- pinned rather than argued.
TEST_CASE("path::stem and extension answer a separator-less root", "[path]")
{
#ifdef _WIN32
    REQUIRE(stem("C:a.txt") == "a");
#else
    REQUIRE(stem("C:a.txt") == "C:a");
#endif
    REQUIRE(extension("C:a.txt") == ".txt");
}

#ifndef _WIN32

/// The other half of the asymmetry #388 settled: a backslash is an ordinary character in a POSIX
/// filename, so none of these spellings names anything but a file whose name contains one.
TEST_CASE("path::treats a backslash as an ordinary filename character", "[path][posix]")
{
    REQUIRE(sv(normalize("..\\victim.txt")) == "..\\victim.txt");
    REQUIRE(sv(normalize("a\\b")) == "a\\b");
    REQUIRE(filename("a\\b") == "a\\b");
    REQUIRE(parent("a\\b") == "");
    REQUIRE(is_normal("a\\b"));
    REQUIRE_FALSE(is_absolute("\\victim.txt"));
    REQUIRE_FALSE(is_absolute("\\\\host\\share\\x"));
    REQUIRE_FALSE(is_absolute("C:a"));
    REQUIRE_FALSE(is_absolute("C:\\a"));
}

#endif

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

/// `C:` and `C:a` are rooted on a drive, not under the build root, so they answer this the same
/// way `C:/a` does; the earlier `REQUIRE_FALSE` pinned the hole #388 was filed for.
TEST_CASE("path::is_absolute on drive-absolute paths", "[path][windows]")
{
    REQUIRE(is_absolute("C:/a"));
    REQUIRE(is_absolute("C:\\a"));
    REQUIRE(is_absolute("C:"));
    REQUIRE(is_absolute("C:a"));
}

TEST_CASE("path::normalize reads a backslash as a separator", "[path][windows]")
{
    SECTION("between names")
    {
        REQUIRE(sv(normalize("a\\b")) == "a/b");
        REQUIRE(sv(normalize("a\\b/c")) == "a/b/c");
    }

    SECTION("a parent reference spelled with a backslash resolves")
    {
        REQUIRE(sv(normalize("a\\..\\b")) == "b");
        REQUIRE(sv(normalize("..\\victim.txt")) == "../victim.txt");
        REQUIRE(sv(normalize("a\\..\\..\\victim.txt")) == "../victim.txt");
    }

    SECTION("dot segments and repeated separators")
    {
        REQUIRE(sv(normalize("a\\.\\\\b")) == "a/b");
        REQUIRE(sv(normalize("a\\")) == "a");
    }
}

TEST_CASE("path::normalize keeps a UNC path rooted", "[path][windows]")
{
    REQUIRE(sv(normalize("\\\\host\\share\\victim.txt")) == "//host/share/victim.txt");
    REQUIRE(sv(normalize("\\\\host\\share")) == "//host/share");

    // Where the parent references resolve to does not matter: staying rooted is what refuses it.
    REQUIRE(is_absolute(sv(normalize("\\\\host\\share\\..\\..\\victim.txt"))));
}

TEST_CASE("path::normalize keeps a drive-relative prefix unseparated", "[path][windows]")
{
    // "C:a" names a's location on C's own current directory; "C:/a" is a different file.
    REQUIRE(sv(normalize("C:a\\b")) == "C:a/b");
    REQUIRE(sv(normalize("C:")) == "C:");
}

TEST_CASE("path::is_absolute on the remaining Windows root spellings", "[path][windows]")
{
    REQUIRE(is_absolute("\\\\host\\share\\x"));
    REQUIRE(is_absolute("\\victim.txt"));
    REQUIRE_FALSE(is_absolute("a\\b"));
    REQUIRE_FALSE(is_absolute("..\\victim.txt"));
}

TEST_CASE("path::parent and filename read a backslash as a separator", "[path][windows]")
{
    REQUIRE(parent("a\\b\\c") == "a\\b");
    REQUIRE(filename("a\\b\\c") == "c");
    REQUIRE(stem("a\\b.txt") == "b");
    REQUIRE(extension("a\\b.txt") == ".txt");
}

TEST_CASE("path::is_normal rejects a backslash-separated spelling", "[path][windows]")
{
    REQUIRE_FALSE(is_normal("a\\b"));
    REQUIRE_FALSE(is_normal("a\\"));
    REQUIRE(is_normal("a/b"));
}

TEST_CASE("path::join does not double a backslash separator", "[path][windows]")
{
    REQUIRE(sv(join("a\\", "b")) == "a\\b");
    REQUIRE(sv(join("x", "\\\\host\\share\\b")) == "\\\\host\\share\\b");
    REQUIRE(sv(join("x", "C:b")) == "C:b");
}

TEST_CASE("path::relative reads a backslash as a separator", "[path][windows]")
{
    REQUIRE(sv(relative("a/b/c", "a/b")) == "c");
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

    // Splitting a normal path and rejoining it reproduces it. This is what reaches the three
    // functions that honour a root only by finding a separator inside it (#388).
    REQUIRE(sv(join(parent(once), filename(once))) == once);

    auto const absolute = is_absolute(p);

    // Stated apart from the rejoin above, which cannot see this: join("", x) returns x, so an
    // under-clamped parent and the whole name it leaves behind cancel exactly (#388).
    if (absolute) {
        REQUIRE(is_absolute(parent(once)));
    }

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

    SECTION("drive-relative paths")
    {
        check_laws_under_root("C:");
    }

    // The root a UNC path normalizes onto. The authority that follows it is ordinary components,
    // which is why the laws are stated against "//" and not against "//host/share" (#388).
    SECTION("UNC-rooted paths")
    {
        check_laws_under_root("//");
    }
#endif
}
