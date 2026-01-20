// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"

#include "pup/parser/depfile.hpp"

using namespace pup::parser;

TEST_CASE("Depfile: parse simple single-line", "[depfile]")
{
    auto content = std::string_view { "foo.o: foo.c header.h\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(result->target == "foo.o");
    REQUIRE(result->dependencies.size() == 2);
    CHECK(result->dependencies[0] == "foo.c");
    CHECK(result->dependencies[1] == "header.h");
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
    CHECK(result->target == "foo.o");
    REQUIRE(result->dependencies.size() == 3);
    CHECK(result->dependencies[0] == "foo.c");
    CHECK(result->dependencies[1] == "header1.h");
    CHECK(result->dependencies[2] == "header2.h");
}

TEST_CASE("Depfile: handle escaped spaces in paths", "[depfile]")
{
    auto content = std::string_view { "foo.o: path\\ with\\ spaces/file.c\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(result->target == "foo.o");
    REQUIRE(result->dependencies.size() == 1);
    CHECK(result->dependencies[0] == "path with spaces/file.c");
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
    CHECK(result->target == "build/main.o");
    REQUIRE(result->dependencies.size() == 6);
    CHECK(result->dependencies[0] == "src/main.cpp");
    CHECK(result->dependencies[1] == "include/pup/core/result.hpp");
    CHECK(result->dependencies[2] == "include/pup/parser/parser.hpp");
    CHECK(result->dependencies[3] == "include/pup/graph/dag.hpp");
    CHECK(result->dependencies[4] == "/usr/include/c++/13/iostream");
    CHECK(result->dependencies[5] == "/usr/include/c++/13/string");
}

TEST_CASE("Depfile: handle no dependencies", "[depfile]")
{
    auto content = std::string_view { "foo.o:\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(result->target == "foo.o");
    CHECK(result->dependencies.empty());
}

TEST_CASE("Depfile: handle colon attached to target", "[depfile]")
{
    auto content = std::string_view { "foo.o:foo.c\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(result->target == "foo.o");
    REQUIRE(result->dependencies.size() == 1);
    CHECK(result->dependencies[0] == "foo.c");
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
    CHECK(result->target == "foo.o");
    REQUIRE(result->dependencies.size() == 2);
    CHECK(result->dependencies[0] == "foo.c");
    CHECK(result->dependencies[1] == "header.h");
}

TEST_CASE("Depfile: handle multiple spaces between deps", "[depfile]")
{
    auto content = std::string_view { "foo.o: foo.c    header1.h   header2.h\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(result->target == "foo.o");
    REQUIRE(result->dependencies.size() == 3);
}

TEST_CASE("Depfile: handle tabs", "[depfile]")
{
    auto content = std::string_view { "foo.o:\tfoo.c\theader.h\n" };
    auto result = parse_depfile(content);

    REQUIRE(result.has_value());
    CHECK(result->target == "foo.o");
    REQUIRE(result->dependencies.size() == 2);
}
