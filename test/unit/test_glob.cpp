// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "temp_root.hpp"

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/glob.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace pup::parser;
using pup::StringId;
using pup::global_pool;

namespace {
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }

/// RAII helper to create a temporary directory tree for testing
class TempDir {
public:
    // Test shards run as concurrent processes, so the name must be unique across them.
    TempDir()
    {
        path_ = pup::test::temp_dir("pup_glob");
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempDir(TempDir const&) = delete;
    auto operator=(TempDir const&) -> TempDir& = delete;

    [[nodiscard]] auto path() const -> fs::path const& { return path_; }

    auto create_file(std::string_view rel) -> void
    {
        std::ofstream { path_ / rel };
    }

private:
    fs::path path_;
};
} // namespace

TEST_CASE("Glob pattern matching", "[glob]")
{
    SECTION("literal matching")
    {
        auto glob = Glob{"foo.c"};
        REQUIRE(glob.is_literal());
        REQUIRE(glob.matches("foo.c"));
        REQUIRE_FALSE(glob.matches("bar.c"));
    }

    SECTION("star matches any characters")
    {
        auto glob = Glob{"*.c"};
        REQUIRE_FALSE(glob.is_literal());
        REQUIRE(glob.matches("foo.c"));
        REQUIRE(glob.matches("bar.c"));
        REQUIRE(glob.matches(".c"));
        REQUIRE_FALSE(glob.matches("foo.h"));
    }

    SECTION("star does not match slash")
    {
        auto glob = Glob{"*.c"};
        REQUIRE_FALSE(glob.matches("src/foo.c"));
    }

    SECTION("question mark matches single character")
    {
        auto glob = Glob{"foo?.c"};
        REQUIRE(glob.matches("foo1.c"));
        REQUIRE(glob.matches("fooX.c"));
        REQUIRE_FALSE(glob.matches("foo.c"));
        REQUIRE_FALSE(glob.matches("foo12.c"));
    }

    SECTION("double star matches path segments")
    {
        auto glob = Glob{"**/*.c"};
        REQUIRE(glob.is_recursive());
        REQUIRE(glob.matches("src/foo.c"));
        REQUIRE(glob.matches("src/sub/bar.c"));
        REQUIRE(glob.matches("foo.c"));
    }

    SECTION("bracket character class")
    {
        auto glob = Glob{"foo[abc].c"};
        REQUIRE(glob.matches("fooa.c"));
        REQUIRE(glob.matches("foob.c"));
        REQUIRE(glob.matches("fooc.c"));
        REQUIRE_FALSE(glob.matches("food.c"));
    }

    SECTION("bracket range")
    {
        auto glob = Glob{"foo[0-9].c"};
        REQUIRE(glob.matches("foo0.c"));
        REQUIRE(glob.matches("foo5.c"));
        REQUIRE(glob.matches("foo9.c"));
        REQUIRE_FALSE(glob.matches("fooa.c"));
    }

    SECTION("bracket negation")
    {
        auto glob = Glob{"foo[!0-9].c"};
        REQUIRE(glob.matches("fooa.c"));
        REQUIRE(glob.matches("fooz.c"));
        REQUIRE_FALSE(glob.matches("foo5.c"));
    }

    SECTION("complex pattern")
    {
        auto glob = Glob{"src/**/test_*.cpp"};
        REQUIRE(glob.matches("src/test_foo.cpp"));
        REQUIRE(glob.matches("src/unit/test_bar.cpp"));
        REQUIRE_FALSE(glob.matches("src/foo.cpp"));
    }
}

TEST_CASE("Path utilities", "[glob]")
{
    SECTION("path_basename")
    {
        REQUIRE(path_basename("foo.c") == "foo.c");
        REQUIRE(path_basename("src/foo.c") == "foo.c");
        REQUIRE(path_basename("src/sub/foo.c") == "foo.c");
        REQUIRE(path_basename("") == "");
    }

    SECTION("path_stem")
    {
        REQUIRE(path_stem("foo.c") == "foo");
        REQUIRE(path_stem("foo.bar.c") == "foo.bar");
        REQUIRE(path_stem("src/foo.c") == "foo");
        REQUIRE(path_stem("foo") == "foo");
        REQUIRE(path_stem(".hidden") == ".hidden");
    }

    SECTION("path_extension")
    {
        REQUIRE(path_extension("foo.c") == "c");
        REQUIRE(path_extension("foo.bar.cpp") == "cpp");
        REQUIRE(path_extension("foo") == "");
        REQUIRE(path_extension(".hidden") == "");
    }

    SECTION("path_directory")
    {
        REQUIRE(path_directory("foo.c") == "");
        REQUIRE(path_directory("src/foo.c") == "src");
        REQUIRE(path_directory("src/sub/foo.c") == "src/sub");
    }
}

TEST_CASE("Glob split path", "[glob]")
{
    SECTION("no directory")
    {
        auto [dir, pattern] = glob_split_path("*.c");
        REQUIRE(dir == "");
        REQUIRE(pattern == "*.c");
    }

    SECTION("directory with pattern")
    {
        auto [dir, pattern] = glob_split_path("src/*.c");
        REQUIRE(dir == "src");
        REQUIRE(pattern == "*.c");
    }

    SECTION("nested directory")
    {
        auto [dir, pattern] = glob_split_path("src/core/*.cpp");
        REQUIRE(dir == "src/core");
        REQUIRE(pattern == "*.cpp");
    }

    SECTION("double star pattern")
    {
        auto [dir, pattern] = glob_split_path("src/**/*.c");
        REQUIRE(dir == "src");
        REQUIRE(pattern == "**/*.c");
    }

    SECTION("literal path")
    {
        auto [dir, pattern] = glob_split_path("src/foo.c");
        REQUIRE(dir == "src");
        REQUIRE(pattern == "foo.c");
    }
}

TEST_CASE("has_glob_chars", "[glob]")
{
    REQUIRE(has_glob_chars("*.c"));
    REQUIRE(has_glob_chars("foo?.c"));
    REQUIRE(has_glob_chars("[abc].c"));
    REQUIRE_FALSE(has_glob_chars("foo.c"));
    REQUIRE_FALSE(has_glob_chars("src/foo.c"));
}

TEST_CASE("glob_match_extract", "[glob]")
{
    SECTION("simple extension pattern")
    {
        REQUIRE(sv(glob_match_extract("*.c", "hello.c")) == "hello");
        REQUIRE(sv(glob_match_extract("*.cpp", "foo.cpp")) == "foo");
    }

    SECTION("suffix pattern")
    {
        REQUIRE(sv(glob_match_extract("*_test.c", "foo_test.c")) == "foo");
        REQUIRE(sv(glob_match_extract("*_test.c", "bar_baz_test.c")) == "bar_baz");
    }

    SECTION("prefix pattern")
    {
        REQUIRE(sv(glob_match_extract("test_*.c", "test_foo.c")) == "foo");
        REQUIRE(sv(glob_match_extract("lib*.so", "libfoo.so")) == "foo");
    }

    SECTION("prefix and suffix pattern")
    {
        REQUIRE(sv(glob_match_extract("test_*.out", "test_hello.out")) == "hello");
    }

    SECTION("path-based patterns")
    {
        REQUIRE(sv(glob_match_extract("src/*.c", "src/foo.c")) == "foo");
        REQUIRE(sv(glob_match_extract("lib/*_test.c", "lib/bar_test.c")) == "bar");
    }

    SECTION("no wildcard returns empty")
    {
        REQUIRE(sv(glob_match_extract("foo.c", "foo.c")) == "");
        REQUIRE(sv(glob_match_extract("src/bar.c", "src/bar.c")) == "");
    }

    SECTION("non-matching returns empty")
    {
        REQUIRE(sv(glob_match_extract("*.c", "foo.cpp")) == "");
        REQUIRE(sv(glob_match_extract("test_*.c", "foo.c")) == "");
    }

    SECTION("double star treated as single")
    {
        REQUIRE(sv(glob_match_extract("**.c", "foo.c")) == "foo");
    }
}

SCENARIO("glob expansion orders matches by path, not by interning order", "[glob]")
{
    GIVEN("a directory tree whose paths were interned in reverse-lexicographic order")
    {
        // Names unique to this test, so the interning below decides their handles.
        auto const paths = std::array<std::string_view, 5> {
            "gord_alpha.txt",
            "gord_bravo.txt",
            "gord_sub/gord_charlie.txt",
            "gord_sub/gord_mike.txt",
            "gord_zeta.txt",
        };

        auto tmp = TempDir {};
        fs::create_directory(tmp.path() / "gord_sub");
        for (auto path : paths) {
            tmp.create_file(path);
        }
        for (auto i = paths.size(); i-- > 0;) {
            (void)global_pool().intern(paths[i]);
        }

        auto expanded = [&](std::string_view pattern) {
            auto matches = glob_expand(pattern, tmp.path().string());
            REQUIRE(matches.has_value());
            auto result = std::vector<std::string_view> {};
            for (auto id : *matches) {
                result.push_back(sv(id));
            }
            return result;
        };

        WHEN("a plain pattern is expanded")
        {
            THEN("the matches are in lexicographic path order")
            {
                REQUIRE(expanded("*.txt") == std::vector<std::string_view> { paths[0], paths[1], paths[4] });
            }
        }

        WHEN("a recursive pattern is expanded")
        {
            THEN("the matches are in lexicographic path order")
            {
                REQUIRE(expanded("**/*.txt") == std::vector<std::string_view>(paths.begin(), paths.end()));
            }
        }
    }
}
