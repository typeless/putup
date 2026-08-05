// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/depfile.hpp"

using namespace pup::parser;
using pup::StringId;
using pup::global_pool;

namespace {
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }
} // namespace

TEST_CASE("Depfile: parse simple single-line", "[depfile]")
{
    auto content = std::string_view { "foo.o: foo.c header.h\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    REQUIRE(result->dependencies.size() == 2);
    CHECK(sv(result->dependencies[0]) == "foo.c");
    CHECK(sv(result->dependencies[1]) == "header.h");
}

TEST_CASE("Depfile: parse multiline with backslash continuation", "[depfile]")
{
    auto content = std::string_view {
        "foo.o: foo.c \\\n"
        "  header1.h \\\n"
        "  header2.h\n"
    };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    REQUIRE(result->dependencies.size() == 3);
    CHECK(sv(result->dependencies[0]) == "foo.c");
    CHECK(sv(result->dependencies[1]) == "header1.h");
    CHECK(sv(result->dependencies[2]) == "header2.h");
}

TEST_CASE("Depfile: handle escaped spaces in paths", "[depfile]")
{
    auto content = std::string_view { "foo.o: path\\ with\\ spaces/file.c\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    REQUIRE(result->dependencies.size() == 1);
    CHECK(sv(result->dependencies[0]) == "path with spaces/file.c");
}

TEST_CASE("Depfile: parse real gcc output", "[depfile]")
{
    auto content = std::string_view {
        "build/main.o: src/main.cpp include/pup/core/result.hpp \\\n"
        " include/pup/parser/parser.hpp include/pup/graph/dag.hpp \\\n"
        " /usr/include/c++/13/iostream /usr/include/c++/13/string\n"
    };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "build/main.o");
    REQUIRE(result->dependencies.size() == 6);
    CHECK(sv(result->dependencies[0]) == "src/main.cpp");
    CHECK(sv(result->dependencies[1]) == "include/pup/core/result.hpp");
    CHECK(sv(result->dependencies[2]) == "include/pup/parser/parser.hpp");
    CHECK(sv(result->dependencies[3]) == "include/pup/graph/dag.hpp");
    CHECK(sv(result->dependencies[4]) == "/usr/include/c++/13/iostream");
    CHECK(sv(result->dependencies[5]) == "/usr/include/c++/13/string");
}

TEST_CASE("Depfile: collect dependencies from every rule", "[depfile]")
{
    auto content = std::string_view {
        "a.o: a.c a.h\n"
        "b.o: b.c b.h\n"
    };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "a.o");
    REQUIRE(result->dependencies.size() == 4);
    CHECK(sv(result->dependencies[0]) == "a.c");
    CHECK(sv(result->dependencies[1]) == "a.h");
    CHECK(sv(result->dependencies[2]) == "b.c");
    CHECK(sv(result->dependencies[3]) == "b.h");
}

TEST_CASE("Depfile: parse real multi-source gcc output", "[depfile]")
{
    auto content = std::string_view {
        "a.o: a.c /usr/include/stdc-predef.h \\\n"
        " a.h\n"
        "b.o: b.c /usr/include/stdc-predef.h \\\n"
        " b.h\n"
    };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "a.o");
    REQUIRE(result->dependencies.size() == 6);
    CHECK(sv(result->dependencies[2]) == "a.h");
    CHECK(sv(result->dependencies[3]) == "b.c");
    CHECK(sv(result->dependencies[5]) == "b.h");
}

TEST_CASE("Depfile: keep a dependency repeated across rules", "[depfile]")
{
    // Implicit edges are deduped at insertion, so repeats stay in the parse result.
    auto content = std::string_view {
        "a.o: a.c shared.h\n"
        "b.o: b.c shared.h\n"
    };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    REQUIRE(result->dependencies.size() == 4);
    CHECK(sv(result->dependencies[1]) == "shared.h");
    CHECK(sv(result->dependencies[3]) == "shared.h");
}

TEST_CASE("Depfile: skip -MP phony rules", "[depfile]")
{
    auto content = std::string_view {
        "a.o: a.c a.h\n"
        "\n"
        "a.h:\n"
        "\n"
        "b.o: b.c b.h\n"
        "\n"
        "b.h:\n"
    };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "a.o");
    REQUIRE(result->dependencies.size() == 4);
    CHECK(sv(result->dependencies[0]) == "a.c");
    CHECK(sv(result->dependencies[1]) == "a.h");
    CHECK(sv(result->dependencies[2]) == "b.c");
    CHECK(sv(result->dependencies[3]) == "b.h");
}

TEST_CASE("Depfile: handle no dependencies", "[depfile]")
{
    auto content = std::string_view { "foo.o:\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    CHECK(result->dependencies.empty());
}

TEST_CASE("Depfile: handle colon attached to target", "[depfile]")
{
    auto content = std::string_view { "foo.o:foo.c\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    REQUIRE(result->dependencies.size() == 1);
    CHECK(sv(result->dependencies[0]) == "foo.c");
}

TEST_CASE("Depfile: reject empty content", "[depfile]")
{
    auto content = std::string_view { "" };
    auto result = parse_depfile(content);

    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Depfile: handle Windows line endings", "[depfile]")
{
    auto content = std::string_view { "foo.o: foo.c \\\r\n  header.h\r\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    REQUIRE(result->dependencies.size() == 2);
    CHECK(sv(result->dependencies[0]) == "foo.c");
    CHECK(sv(result->dependencies[1]) == "header.h");
}

TEST_CASE("Depfile: handle multiple spaces between deps", "[depfile]")
{
    auto content = std::string_view { "foo.o: foo.c    header1.h   header2.h\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    REQUIRE(result->dependencies.size() == 3);
}

TEST_CASE("Depfile: handle tabs", "[depfile]")
{
    auto content = std::string_view { "foo.o:\tfoo.c\theader.h\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(sv(result->target) == "foo.o");
    REQUIRE(result->dependencies.size() == 2);
}

TEST_CASE("scan output is the rule the scan asked for", "[depfile]")
{
    SECTION("a compiler's dependency rule")
    {
        auto result = parse_depfile("foo.o: foo.c foo.h\n");
        REQUIRE(result);
        REQUIRE(is_scan_output(*result));
    }

    SECTION("a note printed before the rule")
    {
        auto result = parse_depfile("Note: including file: ./foo.h\nfoo.o: foo.c foo.h\n");
        REQUIRE(result);
        REQUIRE_FALSE(is_scan_output(*result));
    }

    SECTION("an object spelled the MSVC way")
    {
        auto result = parse_depfile("foo.obj: foo.cpp\n");
        REQUIRE(result);
        REQUIRE(is_scan_output(*result));
    }
}
