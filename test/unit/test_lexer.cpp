// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/parser/lexer.hpp"

using namespace pup::parser;

TEST_CASE("Lexer basic tokens", "[lexer]")
{
    SECTION("empty input")
    {
        auto lexer = Lexer{""};
        REQUIRE(lexer.next().is(TokenType::Eof));
    }

    SECTION("newlines")
    {
        auto lexer = Lexer{"\n\n"};
        REQUIRE(lexer.next().is(TokenType::Newline));
        REQUIRE(lexer.next().is(TokenType::Newline));
        REQUIRE(lexer.next().is(TokenType::Eof));
    }

    SECTION("single character tokens")
    {
        auto lexer = Lexer{": | ( ) { } < > , = @ & $ % ^ !"};
        REQUIRE(lexer.next().is(TokenType::Colon));
        REQUIRE(lexer.next().is(TokenType::Pipe));
        REQUIRE(lexer.next().is(TokenType::OpenParen));
        REQUIRE(lexer.next().is(TokenType::CloseParen));
        REQUIRE(lexer.next().is(TokenType::OpenBrace));
        REQUIRE(lexer.next().is(TokenType::CloseBrace));
        REQUIRE(lexer.next().is(TokenType::OpenAngle));
        REQUIRE(lexer.next().is(TokenType::CloseAngle));
        REQUIRE(lexer.next().is(TokenType::Comma));
        REQUIRE(lexer.next().is(TokenType::Equals));
        REQUIRE(lexer.next().is(TokenType::At));
        REQUIRE(lexer.next().is(TokenType::Ampersand));
        REQUIRE(lexer.next().is(TokenType::Dollar));
        REQUIRE(lexer.next().is(TokenType::Percent));
        REQUIRE(lexer.next().is(TokenType::Caret));
        REQUIRE(lexer.next().is(TokenType::Bang));
    }

    SECTION("multi-character tokens")
    {
        auto lexer = Lexer{"|> := +="};
        REQUIRE(lexer.next().is(TokenType::PipeArrow));
        REQUIRE(lexer.next().is(TokenType::ColonEquals));
        REQUIRE(lexer.next().is(TokenType::PlusEquals));
    }

    SECTION("conditional assignment operators")
    {
        // Use ?\?= to avoid trigraph interpretation (??= -> #)
        auto lexer = Lexer{"?= ?\?="};
        REQUIRE(lexer.next().is(TokenType::QuestionEquals));
        REQUIRE(lexer.next().is(TokenType::DoubleQuestionEquals));
    }

    SECTION("comments")
    {
        auto lexer = Lexer{"# this is a comment\nfoo"};
        REQUIRE(lexer.next().is(TokenType::Hash));
        REQUIRE(lexer.next().is(TokenType::Newline));
        auto tok = lexer.next();
        REQUIRE(tok.is(TokenType::Identifier));
        REQUIRE(tok.text == "foo");
    }
}

TEST_CASE("Lexer keywords", "[lexer]")
{
    auto lexer = Lexer{"foreach include include_rules ifdef ifndef ifeq ifneq else endif export import"};

    REQUIRE(lexer.next().is(TokenType::KwForeach));
    REQUIRE(lexer.next().is(TokenType::KwInclude));
    REQUIRE(lexer.next().is(TokenType::KwIncludeRules));
    REQUIRE(lexer.next().is(TokenType::KwIfdef));
    REQUIRE(lexer.next().is(TokenType::KwIfndef));
    REQUIRE(lexer.next().is(TokenType::KwIfeq));
    REQUIRE(lexer.next().is(TokenType::KwIfneq));
    REQUIRE(lexer.next().is(TokenType::KwElse));
    REQUIRE(lexer.next().is(TokenType::KwEndif));
    REQUIRE(lexer.next().is(TokenType::KwExport));
    REQUIRE(lexer.next().is(TokenType::KwImport));
}

TEST_CASE("Lexer identifiers", "[lexer]")
{
    SECTION("simple identifiers")
    {
        auto lexer = Lexer{"foo bar_baz FOO123"};
        auto tok1 = lexer.next();
        REQUIRE(tok1.is(TokenType::Identifier));
        REQUIRE(tok1.text == "foo");

        auto tok2 = lexer.next();
        REQUIRE(tok2.is(TokenType::Identifier));
        REQUIRE(tok2.text == "bar_baz");

        auto tok3 = lexer.next();
        REQUIRE(tok3.is(TokenType::Identifier));
        REQUIRE(tok3.text == "FOO123");
    }

    SECTION("identifier vs keyword")
    {
        auto lexer = Lexer{"foreach foreachx"};
        REQUIRE(lexer.next().is(TokenType::KwForeach));

        auto tok = lexer.next();
        REQUIRE(tok.is(TokenType::Identifier));
        REQUIRE(tok.text == "foreachx");
    }
}

TEST_CASE("Lexer text and paths", "[lexer]")
{
    SECTION("simple paths")
    {
        auto lexer = Lexer{"src/foo.c ../bar.h ./baz"};
        auto tok1 = lexer.next();
        REQUIRE(tok1.is(TokenType::Text));
        REQUIRE(tok1.text == "src/foo.c");

        auto tok2 = lexer.next();
        REQUIRE(tok2.is(TokenType::Text));
        REQUIRE(tok2.text == "../bar.h");

        auto tok3 = lexer.next();
        REQUIRE(tok3.is(TokenType::Text));
        REQUIRE(tok3.text == "./baz");
    }

    SECTION("glob patterns")
    {
        auto lexer = Lexer{"*.c foo?.h [a-z]*.o"};
        auto tok1 = lexer.next();
        REQUIRE(tok1.text == "*.c");

        auto tok2 = lexer.next();
        REQUIRE(tok2.text == "foo?.h");

        auto tok3 = lexer.next();
        // Note: [a-z] is tokenized separately due to brackets
        REQUIRE(tok3.text == "[");
    }

    SECTION("pattern flags")
    {
        auto lexer = Lexer{"%f %B.o %o"};
        auto tok1 = lexer.next();
        REQUIRE(tok1.text == "%f");

        auto tok2 = lexer.next();
        REQUIRE(tok2.text == "%B.o");

        auto tok3 = lexer.next();
        REQUIRE(tok3.text == "%o");
    }
}

TEST_CASE("Lexer strings", "[lexer]")
{
    SECTION("double quoted")
    {
        auto lexer = Lexer{"\"hello world\""};
        auto tok = lexer.next();
        REQUIRE(tok.is(TokenType::String));
        REQUIRE(tok.text == "\"hello world\"");
    }

    SECTION("single quoted")
    {
        auto lexer = Lexer{"'hello world'"};
        auto tok = lexer.next();
        REQUIRE(tok.is(TokenType::String));
        REQUIRE(tok.text == "'hello world'");
    }

    SECTION("escaped quotes")
    {
        auto lexer = Lexer{"\"hello \\\"world\\\"\""};
        auto tok = lexer.next();
        REQUIRE(tok.is(TokenType::String));
        REQUIRE(tok.text == "\"hello \\\"world\\\"\"");
    }
}

TEST_CASE("Lexer line continuation", "[lexer]")
{
    auto lexer = Lexer{"foo \\\nbar"};
    auto tok1 = lexer.next();
    REQUIRE(tok1.is(TokenType::Identifier));
    REQUIRE(tok1.text == "foo");

    auto tok2 = lexer.next();
    REQUIRE(tok2.is(TokenType::Identifier));
    REQUIRE(tok2.text == "bar");
}

TEST_CASE("Lexer source location", "[lexer]")
{
    auto lexer = Lexer{"foo\nbar baz", "test.tup"};

    auto tok1 = lexer.next();
    REQUIRE(tok1.location.filename == "test.tup");
    REQUIRE(tok1.location.line == 1);
    REQUIRE(tok1.location.column == 1);

    auto newline = lexer.next();
    REQUIRE(newline.is(TokenType::Newline));

    auto tok2 = lexer.next();
    REQUIRE(tok2.location.line == 2);
    REQUIRE(tok2.location.column == 1);

    auto tok3 = lexer.next();
    REQUIRE(tok3.location.line == 2);
    REQUIRE(tok3.location.column == 5);
}

TEST_CASE("Lexer peek", "[lexer]")
{
    auto lexer = Lexer{"foo bar"};

    auto peek1 = lexer.peek();
    REQUIRE(peek1.text == "foo");

    auto peek2 = lexer.peek();
    REQUIRE(peek2.text == "foo"); // Same token

    auto next1 = lexer.next();
    REQUIRE(next1.text == "foo");

    auto next2 = lexer.next();
    REQUIRE(next2.text == "bar");
}

TEST_CASE("Lexer rule line", "[lexer]")
{
    auto lexer = Lexer{": foreach *.c |> gcc -c %f -o %o |> %B.o {objs}"};

    REQUIRE(lexer.next().is(TokenType::Colon));
    REQUIRE(lexer.next().is(TokenType::KwForeach));

    auto pattern = lexer.next();
    REQUIRE(pattern.text == "*.c");

    REQUIRE(lexer.next().is(TokenType::PipeArrow));

    // Command context - would normally switch context here
    lexer.set_context(Lexer::Context::Command);
    auto cmd = lexer.next();
    REQUIRE(cmd.is(TokenType::Text));
    // Command text runs until |>

    REQUIRE(lexer.next().is(TokenType::PipeArrow));

    lexer.set_context(Lexer::Context::Outputs);
    auto output = lexer.next();
    REQUIRE(output.text == "%B.o");

    REQUIRE(lexer.next().is(TokenType::OpenBrace));

    auto group = lexer.next();
    REQUIRE(group.text == "objs");

    REQUIRE(lexer.next().is(TokenType::CloseBrace));
}
