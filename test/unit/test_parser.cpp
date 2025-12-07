// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/parser/parser.hpp"

using namespace pup::parser;

TEST_CASE("Parser empty file", "[parser]")
{
    auto parser = Parser{"", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    REQUIRE(result->statements.empty());
}

TEST_CASE("Parser comments and blank lines", "[parser]")
{
    auto parser = Parser{"# comment\n\n# another\n", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    REQUIRE(result->statements.empty());
}

TEST_CASE("Parser simple assignment", "[parser]")
{
    SECTION("set")
    {
        auto parser = Parser{"FOO = bar", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        REQUIRE(result->statements.size() == 1);
        REQUIRE(result->statements[0]->is<Assignment>());

        auto const* assign = result->statements[0]->as<Assignment>();
        REQUIRE(assign->name == "FOO");
        REQUIRE(assign->op == Assignment::Op::Set);
        REQUIRE(assign->value.is_literal());
        REQUIRE(assign->value.as_literal() == "bar");
    }

    SECTION("append")
    {
        auto parser = Parser{"CFLAGS += -Wall", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        REQUIRE(result->statements.size() == 1);

        auto const* assign = result->statements[0]->as<Assignment>();
        REQUIRE(assign->name == "CFLAGS");
        REQUIRE(assign->op == Assignment::Op::Append);
        REQUIRE(assign->value.as_literal() == "-Wall");
    }

    SECTION("define (no expansion)")
    {
        auto parser = Parser{"CC := gcc", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        auto const* assign = result->statements[0]->as<Assignment>();
        REQUIRE(assign->op == Assignment::Op::Define);
    }
}

TEST_CASE("Parser variable references in assignment", "[parser]")
{
    auto parser = Parser{"LDFLAGS = $(CFLAGS) -lm", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* assign = result->statements[0]->as<Assignment>();
    REQUIRE(assign->value.parts.size() == 2);

    // First part is variable reference
    REQUIRE(std::holds_alternative<Expression::Variable>(assign->value.parts[0]));
    auto const& var = std::get<Expression::Variable>(assign->value.parts[0]);
    REQUIRE(var.ref.kind == VarRef::Kind::Regular);
    REQUIRE(var.ref.name == "CFLAGS");

    // Second part is literal
    REQUIRE(std::holds_alternative<Expression::Literal>(assign->value.parts[1]));
}

TEST_CASE("Parser include directives", "[parser]")
{
    SECTION("include_rules")
    {
        auto parser = Parser{"include_rules", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        REQUIRE(result->statements.size() == 1);
        REQUIRE(result->statements[0]->is<Include>());

        auto const* inc = result->statements[0]->as<Include>();
        REQUIRE(inc->is_rules);
    }

    SECTION("include path")
    {
        auto parser = Parser{"include foo/bar.tup", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        auto const* inc = result->statements[0]->as<Include>();
        REQUIRE_FALSE(inc->is_rules);
        REQUIRE(inc->path.as_literal() == "foo/bar.tup");
    }
}

TEST_CASE("Parser export/import", "[parser]")
{
    SECTION("export")
    {
        auto parser = Parser{"export PATH", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        auto const* exp = result->statements[0]->as<Export>();
        REQUIRE(exp->var_name == "PATH");
    }

    SECTION("import without default")
    {
        auto parser = Parser{"import CC", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        auto const* imp = result->statements[0]->as<Import>();
        REQUIRE(imp->var_name == "CC");
        REQUIRE_FALSE(imp->default_value.has_value());
    }

    SECTION("import with default")
    {
        auto parser = Parser{"import CC=gcc", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        auto const* imp = result->statements[0]->as<Import>();
        REQUIRE(imp->var_name == "CC");
        REQUIRE(imp->default_value.has_value());
        REQUIRE(imp->default_value->as_literal() == "gcc");
    }
}

TEST_CASE("Parser conditionals", "[parser]")
{
    SECTION("ifdef")
    {
        auto parser = Parser{"ifdef DEBUG\nCFLAGS += -g\nendif", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        REQUIRE(result->statements.size() == 1);
        REQUIRE(result->statements[0]->is<Conditional>());

        auto const* cond = result->statements[0]->as<Conditional>();
        REQUIRE(cond->kind == Conditional::Kind::Ifdef);
        REQUIRE(cond->var_name == "DEBUG");
        REQUIRE(cond->then_body.size() == 1);
        REQUIRE(cond->else_body.empty());
    }

    SECTION("ifdef with else")
    {
        auto parser = Parser{"ifdef DEBUG\nCFLAGS += -g\nelse\nCFLAGS += -O2\nendif", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        auto const* cond = result->statements[0]->as<Conditional>();
        REQUIRE(cond->then_body.size() == 1);
        REQUIRE(cond->else_body.size() == 1);
    }

    SECTION("ifeq")
    {
        auto parser = Parser{"ifeq ($(CC),gcc)\nCFLAGS += -Wall\nendif", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        auto const* cond = result->statements[0]->as<Conditional>();
        REQUIRE(cond->kind == Conditional::Kind::Ifeq);
        REQUIRE(cond->then_body.size() == 1);
    }
}

TEST_CASE("Parser simple rule", "[parser]")
{
    auto parser = Parser{": foo.c |> gcc -c %f -o %o |> foo.o", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    REQUIRE(result->statements.size() == 1);
    REQUIRE(result->statements[0]->is<Rule>());

    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE_FALSE(rule->foreach_);
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->inputs[0].path.as_literal() == "foo.c");
    REQUIRE(rule->order_only_inputs.empty());
    REQUIRE(rule->outputs.size() == 1);
    REQUIRE(rule->outputs[0].path.as_literal() == "foo.o");
}

TEST_CASE("Parser foreach rule", "[parser]")
{
    auto parser = Parser{": foreach *.c |> gcc -c %f -o %o |> %B.o", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE(rule->foreach_);
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->inputs[0].path.as_literal() == "*.c");
}

TEST_CASE("Parser rule with order-only inputs", "[parser]")
{
    auto parser = Parser{": foo.c | bar.h |> gcc -c %f -o %o |> foo.o", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->order_only_inputs.size() == 1);
    REQUIRE(rule->order_only_inputs[0].path.as_literal() == "bar.h");
}

TEST_CASE("Parser rule with output group", "[parser]")
{
    auto parser = Parser{": foreach *.c |> gcc -c %f -o %o |> %B.o {objs}", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE(rule->output_group.has_value());
    REQUIRE(*rule->output_group == "objs");
}

TEST_CASE("Parser rule with group input", "[parser]")
{
    auto parser = Parser{": {objs} |> ar rcs %o %f |> libfoo.a", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->inputs[0].is_group);
    REQUIRE(rule->inputs[0].group_name == "objs");
}

TEST_CASE("Parser bang macro", "[parser]")
{
    auto parser = Parser{"!cc = |> gcc -c %f -o %o |> %B.o", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    REQUIRE(result->statements.size() == 1);
    REQUIRE(result->statements[0]->is<BangMacro>());

    auto const* macro = result->statements[0]->as<BangMacro>();
    REQUIRE(macro->name == "cc");
    REQUIRE(macro->outputs.size() == 1);
}

TEST_CASE("Parser .gitignore directive", "[parser]")
{
    auto parser = Parser{".gitignore", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    REQUIRE(result->statements.size() == 1);
    REQUIRE(result->statements[0]->is<Gitignore>());
    REQUIRE(result->has_gitignore);
}

TEST_CASE("Parser multiple statements", "[parser]")
{
    auto source = R"(
CC = gcc
CFLAGS = -Wall

!cc = |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o

: foreach *.c |> !cc |> {objs}
: {objs} |> ar rcs %o %f |> libfoo.a
)";

    auto parser = Parser{source, "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    REQUIRE(result->statements.size() == 5);
    REQUIRE(result->statements[0]->is<Assignment>());
    REQUIRE(result->statements[1]->is<Assignment>());
    REQUIRE(result->statements[2]->is<BangMacro>());
    REQUIRE(result->statements[3]->is<Rule>());
    REQUIRE(result->statements[4]->is<Rule>());
}
