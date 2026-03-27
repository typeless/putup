// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/string_pool.hpp"

#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

using pup::StringId;
using pup::global_pool;

namespace {
auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }
} // namespace

TEST_CASE("is_path_under checks path containment", "[path_utils]")
{
    SECTION("path directly under root")
    {
        REQUIRE(pup::is_path_under(std::string {"/root/file.c" }, std::string {"/root" }));
        REQUIRE(pup::is_path_under(std::string {"/root/dir/file.c" }, std::string {"/root" }));
    }

    SECTION("path equals root")
    {
        REQUIRE(pup::is_path_under(std::string {"/root" }, std::string {"/root" }));
    }

    SECTION("path not under root")
    {
        REQUIRE_FALSE(pup::is_path_under(std::string {"/other/file.c" }, std::string {"/root" }));
    }

    SECTION("handles trailing slashes on root")
    {
        REQUIRE(pup::is_path_under(std::string {"/root/file.c" }, std::string {"/root/" }));
    }

    SECTION("handles relative paths")
    {
        REQUIRE(pup::is_path_under(std::string {"src/lib/file.c" }, std::string {"src" }));
        REQUIRE(pup::is_path_under(std::string {"src/lib/file.c" }, std::string {"src/lib" }));
        REQUIRE_FALSE(pup::is_path_under(std::string {"src/lib/file.c" }, std::string {"other" }));
    }

    SECTION("handles directory boundary correctly")
    {
        // "src-new" should not be under "src"
        REQUIRE_FALSE(pup::is_path_under(std::string {"/root/src-new/file.c" }, std::string {"/root/src" }));
        REQUIRE(pup::is_path_under(std::string {"/root/src/file.c" }, std::string {"/root/src" }));
    }
}

TEST_CASE("relative_to_root computes relative paths", "[path_utils]")
{
    SECTION("path under root")
    {
        REQUIRE(sv(pup::relative_to_root(std::string {"/root/src/file.c" }, std::string {"/root" })) == "src/file.c");
        REQUIRE(sv(pup::relative_to_root(std::string {"/root/file.c" }, std::string {"/root" })) == "file.c");
    }

    SECTION("path equals root returns empty")
    {
        REQUIRE(sv(pup::relative_to_root(std::string {"/root" }, std::string {"/root" })).empty());
    }

    SECTION("path not under root returns empty")
    {
        REQUIRE(sv(pup::relative_to_root(std::string {"/other/file.c" }, std::string {"/root" })).empty());
    }

    SECTION("handles trailing slashes")
    {
        REQUIRE(sv(pup::relative_to_root(std::string {"/root/src/file.c" }, std::string {"/root/" })) == "src/file.c");
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
    auto scopes = pup::Vec<pup::StringId> { intern("lib"), intern("app") };

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

TEST_CASE("strip_path_prefix removes prefix from path", "[path_utils][path]")
{
    SECTION("strips matching prefix")
    {
        REQUIRE(sv(pup::strip_path_prefix("build/src/foo.o", "build")) == "src/foo.o");
        REQUIRE(sv(pup::strip_path_prefix("build/include/bar.h", "build")) == "include/bar.h");
    }

    SECTION("handles multi-component prefix")
    {
        REQUIRE(sv(pup::strip_path_prefix("build/busybox/src/main.c", "build/busybox")) == "src/main.c");
    }

    SECTION("returns original when prefix doesn't match")
    {
        REQUIRE(sv(pup::strip_path_prefix("src/foo.o", "build")) == "src/foo.o");
        REQUIRE(sv(pup::strip_path_prefix("other/src/foo.o", "build")) == "other/src/foo.o");
    }

    SECTION("returns original when prefix is partial match")
    {
        REQUIRE(sv(pup::strip_path_prefix("builder/src/foo.o", "build")) == "builder/src/foo.o");
    }

    SECTION("handles empty prefix")
    {
        REQUIRE(sv(pup::strip_path_prefix("src/foo.o", "")) == "src/foo.o");
    }

    SECTION("handles path equal to prefix")
    {
        REQUIRE(sv(pup::strip_path_prefix("build", "build")) == "build");
    }
}

TEST_CASE("resolve_under_root resolves paths to target root", "[path_utils][path]")
{
    auto source_root = std::string {"/home/user/src/project" };
    auto target_root = std::string {"/home/user/src/project/../build" };

    SECTION("resolves path under target root")
    {
        auto result = pup::resolve_under_root("../build/include/foo.h", source_root, target_root);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "include/foo.h");
    }

    SECTION("returns nullopt for non-dotdot paths")
    {
        auto result = pup::resolve_under_root("src/foo.c", source_root, target_root);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("returns nullopt for paths not under target root")
    {
        auto result = pup::resolve_under_root("../other/file.c", source_root, target_root);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("handles deeply nested paths")
    {
        auto result = pup::resolve_under_root("../build/src/lib/deep/file.o", source_root, target_root);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "src/lib/deep/file.o");
    }

    SECTION("handles complex cross-project paths")
    {
        // Scenario: building busybox from pup, where:
        // - source_root = /home/user/src/busybox
        // - target_root = /home/user/src/pup/build/busybox
        // After joining "applets/../../pup/build/busybox/include/autoconf.h" with
        // current_dir and lexically_normal, we get "../pup/build/busybox/include/autoconf.h"
        auto src = std::string {"/home/user/src/busybox" };
        auto tgt = std::string {"/home/user/src/pup/build/busybox" };
        auto result = pup::resolve_under_root("../pup/build/busybox/include/autoconf.h", src, tgt);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "include/autoconf.h");
    }
}
