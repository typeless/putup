// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/graph/scanners/dep_words.hpp"

#include <cstdint>
#include <string>
#include <vector>

using pup::core::tokenize_shell_command;

namespace {
auto sv(pup::core::ShellToken const& token) -> std::string_view { return pup::global_pool().get(token.text); }

class Lcg {
public:
    explicit Lcg(std::uint64_t seed)
        : state_ { seed }
    {
    }

    auto next(std::uint32_t bound) -> std::uint32_t
    {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33) % bound;
    }

private:
    std::uint64_t state_;
};
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

    SECTION("only whitespace")
    {
        auto const result = tokenize_shell_command("   ");
        REQUIRE(result.empty());
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

TEST_CASE("tokenize_shell_command splits a control operator from the word it touches", "[string_utils]")
{
    SECTION("a terminator after a word")
    {
        auto const result = tokenize_shell_command("gcc -c a.c -o a.o; rm x");
        REQUIRE(result.size() == 8);
        REQUIRE(sv(result[4]) == "a.o");
        REQUIRE(sv(result[5]) == ";");
        REQUIRE(sv(result[6]) == "rm");
    }

    SECTION("two-character operators between words")
    {
        auto const result = tokenize_shell_command("a&&b||c");
        REQUIRE(result.size() == 5);
        REQUIRE(sv(result[0]) == "a");
        REQUIRE(sv(result[1]) == "&&");
        REQUIRE(sv(result[2]) == "b");
        REQUIRE(sv(result[3]) == "||");
        REQUIRE(sv(result[4]) == "c");
    }

    SECTION("a pipe and a backgrounding operator")
    {
        auto const result = tokenize_shell_command("a|b&");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[1]) == "|");
        REQUIRE(sv(result[3]) == "&");
    }

    SECTION("a quoted or escaped operator stays in its word")
    {
        auto const result = tokenize_shell_command(R"(echo 'a;b' "c&&d" e\|f)");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[1]) == "a;b");
        REQUIRE(sv(result[2]) == "c&&d");
        REQUIRE(sv(result[3]) == "e|f");
    }

    SECTION("a redirection keeps the operator it is spelled with")
    {
        auto const result = tokenize_shell_command("cmd 2>&1 >|log");
        REQUIRE(result.size() == 3);
        REQUIRE(sv(result[1]) == "2>&1");
        REQUIRE(sv(result[2]) == ">|log");
    }
}

TEST_CASE("tokenize_shell_command splits a redirection from the word it touches", "[string_utils]")
{
    SECTION("an output redirection after a word")
    {
        auto const result = tokenize_shell_command("gcc -o a.o>log");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[2]) == "a.o");
        REQUIRE(sv(result[3]) == ">log");
    }

    SECTION("an input redirection and an appending one")
    {
        auto const result = tokenize_shell_command("cmd a<in b>>out");
        REQUIRE(result.size() == 5);
        REQUIRE(sv(result[1]) == "a");
        REQUIRE(sv(result[2]) == "<in");
        REQUIRE(sv(result[3]) == "b");
        REQUIRE(sv(result[4]) == ">>out");
    }

    SECTION("a file descriptor number stays with its redirection")
    {
        auto const result = tokenize_shell_command("cmd 2>err >log2>&1");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[1]) == "2>err");
        REQUIRE(sv(result[2]) == ">log2");
        REQUIRE(sv(result[3]) == ">&1");
    }

    SECTION("a quoted number is a word and not a file descriptor")
    {
        auto const result = tokenize_shell_command("cmd '2'>log");
        REQUIRE(result.size() == 3);
        REQUIRE(sv(result[1]) == "2");
        REQUIRE(sv(result[2]) == ">log");
    }

    SECTION("a quoted angle bracket stays in its word")
    {
        auto const result = tokenize_shell_command(R"(echo 'a>b' "c<d" e\>f)");
        REQUIRE(result.size() == 4);
        REQUIRE(sv(result[1]) == "a>b");
        REQUIRE(sv(result[2]) == "c<d");
        REQUIRE(sv(result[3]) == "e>f");
    }
}

TEST_CASE("tokenize_shell_command tells an operator from a word spelled the same", "[string_utils]")
{
    auto const result = tokenize_shell_command(R"(a ; ';' \; "&&" && 2>&1)");
    REQUIRE(result.size() == 7);
    REQUIRE(result[1].kind == pup::core::ShellTokenKind::Operator);
    REQUIRE(result[2].kind == pup::core::ShellTokenKind::Word);
    REQUIRE(result[3].kind == pup::core::ShellTokenKind::Word);
    REQUIRE(result[4].kind == pup::core::ShellTokenKind::Word);
    REQUIRE(result[5].kind == pup::core::ShellTokenKind::Operator);
    REQUIRE(result[6].kind == pup::core::ShellTokenKind::Word);
    REQUIRE(sv(result[6]) == "2>&1");
}

SCENARIO("A quoted word and a bare operator tokenize back to themselves", "[string_utils][property]")
{
    GIVEN("commands of quoted words and bare operators")
    {
        auto const seed = GENERATE(1U, 2U, 3U, 12345U, 999983U);
        auto rng = Lcg { seed };
        auto const alphabet = std::string_view { "ab12;&|<> '\"\\$\n" };
        auto const operators = std::vector<std::string_view> { ";", "&", "&&", "|", "||", "\n" };

        WHEN("each command is rendered and tokenized")
        {
            auto failures = 0;
            auto first_failure = std::string {};

            for (auto trial = 0; trial < 512; ++trial) {
                auto expected = std::vector<std::pair<std::string, pup::core::ShellTokenKind>> {};
                auto rendered = pup::Buf {};
                auto previous_was_operator = true;
                auto const count = 1 + rng.next(6);
                for (auto item = 0U; item < count; ++item) {
                    auto const is_operator = !previous_was_operator && rng.next(3) == 0;
                    if (is_operator) {
                        auto const op = operators[rng.next(static_cast<std::uint32_t>(operators.size()))];
                        if (rng.next(2) == 0) {
                            rendered += ' ';
                        }
                        rendered += op;
                        expected.emplace_back(std::string { op }, pup::core::ShellTokenKind::Operator);
                    } else {
                        auto word = std::string {};
                        auto const length = rng.next(5);
                        for (auto c = 0U; c < length; ++c) {
                            word += alphabet[rng.next(static_cast<std::uint32_t>(alphabet.size()))];
                        }
                        if (!rendered.empty() && (!previous_was_operator || rng.next(2) == 0)) {
                            rendered += ' ';
                        }
                        pup::graph::scanners::shell_quote_into(rendered, word, pup::graph::scanners::QuoteStyle::Posix);
                        expected.emplace_back(word, pup::core::ShellTokenKind::Word);
                    }
                    previous_was_operator = is_operator;
                }

                auto const result = tokenize_shell_command(rendered.view());
                auto matches = result.size() == expected.size();
                for (auto i = std::size_t { 0 }; matches && i < result.size(); ++i) {
                    matches = sv(result[i]) == expected[i].first && result[i].kind == expected[i].second;
                }
                if (!matches) {
                    if (failures == 0) {
                        first_failure = std::string { rendered.view() };
                    }
                    ++failures;
                }
            }

            THEN("every token comes back with its text and its kind")
            {
                INFO("seed " << seed << ", first failing command: " << first_failure);
                REQUIRE(failures == 0);
            }
        }
    }
}

TEST_CASE("tokenize_shell_command ends a command at an unquoted newline", "[string_utils]")
{
    SECTION("a newline between two commands")
    {
        auto const result = tokenize_shell_command("a\nb");
        REQUIRE(result.size() == 3);
        REQUIRE(result[1].kind == pup::core::ShellTokenKind::Operator);
        REQUIRE(sv(result[1]) == "\n");
        REQUIRE(sv(result[2]) == "b");
    }

    SECTION("a quoted newline stays in its word")
    {
        auto const result = tokenize_shell_command("echo 'a\nb'");
        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[1]) == "a\nb");
    }
}

TEST_CASE("tokenize_shell_command keeps a quoted empty word", "[string_utils]")
{
    auto const result = tokenize_shell_command(R"(echo '' "" x)");
    REQUIRE(result.size() == 4);
    REQUIRE(sv(result[1]).empty());
    REQUIRE(result[1].kind == pup::core::ShellTokenKind::Word);
    REQUIRE(sv(result[2]).empty());
    REQUIRE(sv(result[3]) == "x");
}
