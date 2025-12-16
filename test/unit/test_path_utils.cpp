// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/core/path_utils.hpp"

#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("is_path_under checks path containment", "[path_utils]")
{
    SECTION("path directly under root")
    {
        REQUIRE(pup::is_path_under(fs::path { "/root/file.c" }, fs::path { "/root" }));
        REQUIRE(pup::is_path_under(fs::path { "/root/dir/file.c" }, fs::path { "/root" }));
    }

    SECTION("path equals root")
    {
        REQUIRE(pup::is_path_under(fs::path { "/root" }, fs::path { "/root" }));
    }

    SECTION("path not under root")
    {
        REQUIRE_FALSE(pup::is_path_under(fs::path { "/other/file.c" }, fs::path { "/root" }));
    }

    SECTION("handles trailing slashes on root")
    {
        REQUIRE(pup::is_path_under(fs::path { "/root/file.c" }, fs::path { "/root/" }));
    }

    SECTION("handles relative paths")
    {
        REQUIRE(pup::is_path_under(fs::path { "src/lib/file.c" }, fs::path { "src" }));
        REQUIRE(pup::is_path_under(fs::path { "src/lib/file.c" }, fs::path { "src/lib" }));
        REQUIRE_FALSE(pup::is_path_under(fs::path { "src/lib/file.c" }, fs::path { "other" }));
    }

    SECTION("handles directory boundary correctly")
    {
        // "src-new" should not be under "src"
        REQUIRE_FALSE(pup::is_path_under(fs::path { "/root/src-new/file.c" }, fs::path { "/root/src" }));
        REQUIRE(pup::is_path_under(fs::path { "/root/src/file.c" }, fs::path { "/root/src" }));
    }
}

TEST_CASE("relative_to_root computes relative paths", "[path_utils]")
{
    SECTION("path under root")
    {
        REQUIRE(pup::relative_to_root(fs::path { "/root/src/file.c" }, fs::path { "/root" }) == "src/file.c");
        REQUIRE(pup::relative_to_root(fs::path { "/root/file.c" }, fs::path { "/root" }) == "file.c");
    }

    SECTION("path equals root returns empty")
    {
        REQUIRE(pup::relative_to_root(fs::path { "/root" }, fs::path { "/root" }).empty());
    }

    SECTION("path not under root returns empty")
    {
        REQUIRE(pup::relative_to_root(fs::path { "/other/file.c" }, fs::path { "/root" }).empty());
    }

    SECTION("handles trailing slashes")
    {
        REQUIRE(pup::relative_to_root(fs::path { "/root/src/file.c" }, fs::path { "/root/" }) == "src/file.c");
    }
}

TEST_CASE("is_path_in_scope with string prefix matching", "[path_utils]")
{
    SECTION("empty scope matches all")
    {
        REQUIRE(pup::is_path_in_scope("src/lib/file.c", ""));
        REQUIRE(pup::is_path_in_scope("any/path/here", ""));
    }

    SECTION("exact match")
    {
        REQUIRE(pup::is_path_in_scope("lib", "lib"));
    }

    SECTION("path under scope")
    {
        REQUIRE(pup::is_path_in_scope("lib/foo.c", "lib"));
        REQUIRE(pup::is_path_in_scope("lib/sub/bar.c", "lib"));
    }

    SECTION("path not under scope")
    {
        REQUIRE_FALSE(pup::is_path_in_scope("app/main.c", "lib"));
    }

    SECTION("respects directory boundary")
    {
        // "library" should not match scope "lib"
        REQUIRE_FALSE(pup::is_path_in_scope("library/foo.c", "lib"));
        REQUIRE(pup::is_path_in_scope("lib/foo.c", "lib"));
    }

    SECTION("handles both forward and back slashes")
    {
        REQUIRE(pup::is_path_in_scope("lib/foo.c", "lib"));
#ifdef _WIN32
        REQUIRE(pup::is_path_in_scope("lib\\foo.c", "lib"));
#endif
    }
}

TEST_CASE("is_path_in_any_scope with multiple scopes", "[path_utils]")
{
    auto scopes = std::vector<std::string> { "lib", "app" };

    SECTION("path in first scope")
    {
        REQUIRE(pup::is_path_in_any_scope("lib/foo.c", scopes));
    }

    SECTION("path in second scope")
    {
        REQUIRE(pup::is_path_in_any_scope("app/main.c", scopes));
    }

    SECTION("path in neither scope")
    {
        REQUIRE_FALSE(pup::is_path_in_any_scope("test/test.c", scopes));
    }

    SECTION("empty scopes matches all")
    {
        REQUIRE(pup::is_path_in_any_scope("any/path.c", {}));
    }

    SECTION("nested path in scope")
    {
        REQUIRE(pup::is_path_in_any_scope("lib/sub/nested/file.c", scopes));
    }
}
