// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/parser/glob.hpp"

using namespace pup::parser;

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
        REQUIRE(glob_match_extract("*.c", "hello.c") == "hello");
        REQUIRE(glob_match_extract("*.cpp", "foo.cpp") == "foo");
    }

    SECTION("suffix pattern")
    {
        REQUIRE(glob_match_extract("*_test.c", "foo_test.c") == "foo");
        REQUIRE(glob_match_extract("*_test.c", "bar_baz_test.c") == "bar_baz");
    }

    SECTION("prefix pattern")
    {
        REQUIRE(glob_match_extract("test_*.c", "test_foo.c") == "foo");
        REQUIRE(glob_match_extract("lib*.so", "libfoo.so") == "foo");
    }

    SECTION("prefix and suffix pattern")
    {
        REQUIRE(glob_match_extract("test_*.out", "test_hello.out") == "hello");
    }

    SECTION("path-based patterns")
    {
        REQUIRE(glob_match_extract("src/*.c", "src/foo.c") == "foo");
        REQUIRE(glob_match_extract("lib/*_test.c", "lib/bar_test.c") == "bar");
    }

    SECTION("no wildcard returns empty")
    {
        REQUIRE(glob_match_extract("foo.c", "foo.c") == "");
        REQUIRE(glob_match_extract("src/bar.c", "src/bar.c") == "");
    }

    SECTION("non-matching returns empty")
    {
        REQUIRE(glob_match_extract("*.c", "foo.cpp") == "");
        REQUIRE(glob_match_extract("test_*.c", "foo.c") == "");
    }

    SECTION("double star treated as single")
    {
        REQUIRE(glob_match_extract("**.c", "foo.c") == "foo");
    }
}
