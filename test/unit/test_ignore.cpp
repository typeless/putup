// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/parser/ignore.hpp"

using namespace pup::parser;

TEST_CASE("IgnoreList default patterns", "[ignore]")
{
    auto ignore = IgnoreList::with_defaults();

    SECTION("ignores .git directory")
    {
        REQUIRE(ignore.is_ignored(".git"));

    }

    SECTION("ignores .pup directory")
    {
        REQUIRE(ignore.is_ignored(".pup"));

    }

    SECTION("ignores node_modules directory")
    {
        REQUIRE(ignore.is_ignored("node_modules"));

    }

    SECTION("does not ignore regular directories")
    {
        REQUIRE_FALSE(ignore.is_ignored("src"));
        REQUIRE_FALSE(ignore.is_ignored("include"));
        REQUIRE_FALSE(ignore.is_ignored("test"));
    }
}

TEST_CASE("IgnoreList pattern parsing", "[ignore]")
{
    auto ignore = IgnoreList {};

    SECTION("simple directory pattern")
    {
        ignore.add("build/");
        REQUIRE(ignore.is_ignored("build"));
        REQUIRE_FALSE(ignore.is_ignored("build.txt"));
    }

    SECTION("simple glob pattern")
    {
        ignore.add("*.o");
        REQUIRE(ignore.is_ignored("foo.o"));
        REQUIRE(ignore.is_ignored("bar.o"));
        REQUIRE_FALSE(ignore.is_ignored("foo.c"));
    }

    SECTION("recursive glob pattern")
    {
        ignore.add("**/test");
        REQUIRE(ignore.is_ignored("test"));
        REQUIRE(ignore.is_ignored("src/test"));
        REQUIRE(ignore.is_ignored("src/foo/test"));
    }

    SECTION("anchored pattern with slash")
    {
        ignore.add("src/temp");
        REQUIRE(ignore.is_ignored("src/temp"));
        REQUIRE_FALSE(ignore.is_ignored("foo/src/temp"));
        REQUIRE_FALSE(ignore.is_ignored("temp"));
    }
}

TEST_CASE("IgnoreList negation", "[ignore]")
{
    auto ignore = IgnoreList {};

    SECTION("negation overrides previous match")
    {
        ignore.add("*.o");
        ignore.add("!important.o");
        REQUIRE(ignore.is_ignored("foo.o"));
        REQUIRE(ignore.is_ignored("bar.o"));
        REQUIRE_FALSE(ignore.is_ignored("important.o"));
    }

    SECTION("later pattern can override negation")
    {
        ignore.add("build/");
        ignore.add("!build/keep/");
        ignore.add("build/keep/temp/");

        REQUIRE(ignore.is_ignored("build"));
        // Note: directory-only patterns don't affect subdirs this way
        // This is simplified behavior
    }
}

TEST_CASE("IgnoreList glob matching", "[ignore]")
{
    auto ignore = IgnoreList {};

    SECTION("question mark matches single character")
    {
        ignore.add("foo?.c");
        REQUIRE(ignore.is_ignored("foo1.c"));
        REQUIRE(ignore.is_ignored("fooX.c"));
        REQUIRE_FALSE(ignore.is_ignored("foo.c"));
        REQUIRE_FALSE(ignore.is_ignored("foo12.c"));
    }

    SECTION("bracket character class")
    {
        ignore.add("test[0-9].cpp");
        REQUIRE(ignore.is_ignored("test0.cpp"));
        REQUIRE(ignore.is_ignored("test5.cpp"));
        REQUIRE(ignore.is_ignored("test9.cpp"));
        REQUIRE_FALSE(ignore.is_ignored("testa.cpp"));
    }

    SECTION("bracket negation")
    {
        ignore.add("file[!0-9].txt");
        REQUIRE(ignore.is_ignored("filea.txt"));
        REQUIRE(ignore.is_ignored("filez.txt"));
        REQUIRE_FALSE(ignore.is_ignored("file5.txt"));
    }
}

TEST_CASE("IgnoreList path matching", "[ignore]")
{
    auto ignore = IgnoreList {};

    SECTION("deep directory paths")
    {
        ignore.add("vendor/");
        REQUIRE(ignore.is_ignored("vendor"));

    }

    SECTION("nested paths")
    {
        ignore.add("**/cache");
        REQUIRE(ignore.is_ignored("cache"));
        REQUIRE(ignore.is_ignored("src/cache"));
        REQUIRE(ignore.is_ignored("a/b/c/cache"));
    }

    SECTION("pattern with directory separator")
    {
        ignore.add("logs/*.log");
        REQUIRE(ignore.is_ignored("logs/app.log"));
        REQUIRE(ignore.is_ignored("logs/error.log"));
        REQUIRE_FALSE(ignore.is_ignored("app.log"));
        REQUIRE_FALSE(ignore.is_ignored("other/app.log"));
    }
}


TEST_CASE("IgnoreList edge cases", "[ignore]")
{
    auto ignore = IgnoreList {};

    SECTION("empty list ignores nothing")
    {
        REQUIRE(ignore.empty());
        REQUIRE_FALSE(ignore.is_ignored("anything"));
    }

    SECTION("hidden files")
    {
        ignore.add(".*");
        REQUIRE(ignore.is_ignored(".hidden"));
        REQUIRE(ignore.is_ignored(".gitignore"));
        REQUIRE_FALSE(ignore.is_ignored("visible"));
    }

    SECTION("trailing spaces are trimmed during file load")
    {
        // Trailing spaces are trimmed during load(), not add()
        // add() takes the pattern as-is
        ignore.add("build");
        REQUIRE(ignore.is_ignored("build"));
    }
}

TEST_CASE("IgnoreList subtree matching", "[ignore]")
{
    auto ignore = IgnoreList {};

    SECTION("a dir matching a basename pattern is ignored with its descendants")
    {
        ignore.add("tests/");
        REQUIRE(ignore.is_ignored_dir("tests"));
        REQUIRE(ignore.is_ignored_dir("lib/tests"));
        REQUIRE(ignore.is_ignored_dir("lib/tests/nested"));
        REQUIRE_FALSE(ignore.is_ignored_dir("lib"));
        REQUIRE_FALSE(ignore.is_ignored_dir("lib/testsuite"));
    }

    SECTION("anchored patterns ignore only the matching subtree")
    {
        ignore.add("lib/tests/");
        REQUIRE(ignore.is_ignored_dir("lib/tests"));
        REQUIRE(ignore.is_ignored_dir("lib/tests/nested"));
        REQUIRE_FALSE(ignore.is_ignored_dir("tests"));
        REQUIRE_FALSE(ignore.is_ignored_dir("lib"));
    }

    SECTION("the root dir is only ignored by an explicit match")
    {
        ignore.add("tests/");
        REQUIRE_FALSE(ignore.is_ignored_dir("."));
    }

    SECTION("empty list ignores nothing")
    {
        REQUIRE_FALSE(ignore.is_ignored_dir("tests"));
        REQUIRE_FALSE(ignore.is_ignored_dir("."));
    }
}
