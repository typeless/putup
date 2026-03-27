// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/string_utils.hpp"

using pup::core::tokenize_shell_command;

namespace {
auto sv(pup::StringId id) -> std::string_view { return pup::global_pool().get(id); }
} // namespace

TEST_CASE("tokenize_shell_command basic", "[string_utils]")
{
    SECTION("empty string")
    {
        auto const result = tokenize_shell_command("");
        REQUIRE(result.empty());
    }

    SECTION("single word")
    {
        auto const result = tokenize_shell_command("hello");
        REQUIRE(result.size() == 1);
        REQUIRE(sv(result[0]) =="hello");
    }

    SECTION("multiple words")
    {
        auto const result = tokenize_shell_command("echo hello world");
        REQUIRE(result.size() == 3);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello");
        REQUIRE(sv(result[2]) =="world");
    }

    SECTION("multiple spaces between words")
    {
        auto const result = tokenize_shell_command("a   b\tc");
        REQUIRE(result.size() == 3);
        REQUIRE(sv(result[0]) =="a");
        REQUIRE(sv(result[1]) =="b");
        REQUIRE(sv(result[2]) =="c");
    }

    SECTION("leading and trailing whitespace")
    {
        auto const result = tokenize_shell_command("  foo bar  ");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="foo");
        REQUIRE(sv(result[1]) =="bar");
    }
}

TEST_CASE("tokenize_shell_command double quotes", "[string_utils]")
{
    SECTION("simple double quotes")
    {
        auto const result = tokenize_shell_command(R"(echo "hello")");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello");
    }

    SECTION("double quotes with spaces")
    {
        auto const result = tokenize_shell_command(R"(echo "hello world")");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello world");
    }

    SECTION("escaped quote inside double quotes")
    {
        auto const result = tokenize_shell_command(R"(echo "hello \"world\"")");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello \"world\"");
    }

    SECTION("escaped backslash inside double quotes")
    {
        auto const result = tokenize_shell_command(R"(echo "path\\to\\file")");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="path\\to\\file");
    }

    SECTION("mixed escaped and regular inside double quotes")
    {
        auto const result = tokenize_shell_command(R"(echo "a\\b\"c")");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="a\\b\"c");
    }
}

TEST_CASE("tokenize_shell_command single quotes", "[string_utils]")
{
    SECTION("simple single quotes")
    {
        auto const result = tokenize_shell_command("echo 'hello'");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello");
    }

    SECTION("single quotes with spaces")
    {
        auto const result = tokenize_shell_command("echo 'hello world'");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello world");
    }

    SECTION("backslash is literal inside single quotes")
    {
        auto const result = tokenize_shell_command(R"(echo 'hello\nworld')");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello\\nworld");
    }

    SECTION("double quotes are literal inside single quotes")
    {
        auto const result = tokenize_shell_command(R"(echo 'say "hi"')");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="say \"hi\"");
    }
}

TEST_CASE("tokenize_shell_command escape outside quotes", "[string_utils]")
{
    SECTION("escaped space")
    {
        auto const result = tokenize_shell_command(R"(echo hello\ world)");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello world");
    }

    SECTION("escaped backslash")
    {
        auto const result = tokenize_shell_command(R"(echo hello\\world)");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="hello\\world");
    }
}

TEST_CASE("tokenize_shell_command concatenation", "[string_utils]")
{
    SECTION("adjacent quoted strings")
    {
        auto const result = tokenize_shell_command(R"(echo "hello"'world')");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="helloworld");
    }

    SECTION("shell-style single quote escape: it'\\''s")
    {
        auto const result = tokenize_shell_command(R"(echo 'it'\''s')");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) =="echo");
        REQUIRE(sv(result[1]) =="it's");
    }
}

TEST_CASE("tokenize_shell_command compiler commands", "[string_utils]")
{
    SECTION("gcc with define containing spaces")
    {
        auto const result = tokenize_shell_command(R"(gcc -DFOO="bar baz" -c x.c)");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[0]) =="gcc");
        REQUIRE(sv(result[1]) =="-DFOO=bar baz");
        REQUIRE(sv(result[2]) =="-c");
        REQUIRE(sv(result[3]) =="x.c");
    }

    SECTION("complex compile command")
    {
        auto const result = tokenize_shell_command(
            R"(gcc -I/path/to/include -DVERSION="1.0" -c main.c -o main.o)");
        REQUIRE(result.size() == 7);
        REQUIRE(sv(result[0]) =="gcc");
        REQUIRE(sv(result[1]) =="-I/path/to/include");
        REQUIRE(sv(result[2]) =="-DVERSION=1.0");
        REQUIRE(sv(result[3]) =="-c");
        REQUIRE(sv(result[4]) =="main.c");
        REQUIRE(sv(result[5]) =="-o");
        REQUIRE(sv(result[6]) =="main.o");
    }

    SECTION("mbedtls config file with nested quotes")
    {
        // Pattern from spos project: -DMBEDTLS_CONFIG_FILE='"path/to/config.h"'
        // The outer single quotes protect the inner double quotes from shell
        // Result should be: -DMBEDTLS_CONFIG_FILE="path/to/config.h"
        auto const result = tokenize_shell_command(
            R"(gcc -DMBEDTLS_CONFIG_FILE='"../../../include/mbedtls_config.h"' -c x.c)");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[0]) =="gcc");
        REQUIRE(sv(result[1]) ==R"(-DMBEDTLS_CONFIG_FILE="../../../include/mbedtls_config.h")");
        REQUIRE(sv(result[2]) =="-c");
        REQUIRE(sv(result[3]) =="x.c");
    }

    SECTION("define with escaped double quotes")
    {
        // Pattern: -D__PFILENAME__=\"\"
        // Result should be: -D__PFILENAME__=""
        auto const result = tokenize_shell_command(R"(gcc -D__PFILENAME__=\"\" -c x.c)");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[0]) =="gcc");
        REQUIRE(sv(result[1]) ==R"(-D__PFILENAME__="")");
        REQUIRE(sv(result[2]) =="-c");
        REQUIRE(sv(result[3]) =="x.c");
    }
}
