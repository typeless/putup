// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

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
        REQUIRE(assign->name.as_literal() == "FOO");
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
        REQUIRE(assign->name.as_literal() == "CFLAGS");
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

    SECTION("soft set (?=)")
    {
        auto parser = Parser{"FOO ?= default", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        REQUIRE(result->statements.size() == 1);

        auto const* assign = result->statements[0]->as<Assignment>();
        REQUIRE(assign->name.as_literal() == "FOO");
        REQUIRE(assign->op == Assignment::Op::SoftSet);
        REQUIRE(assign->value.as_literal() == "default");
    }

    SECTION("weak set (?\?=)")
    {
        // Use ?\?= to avoid trigraph interpretation (??= -> #)
        auto parser = Parser{"BAR ?\?= fallback", "test.tup"};
        auto result = parser.parse();

        REQUIRE(result.has_value());
        REQUIRE(result->statements.size() == 1);

        auto const* assign = result->statements[0]->as<Assignment>();
        REQUIRE(assign->name.as_literal() == "BAR");
        REQUIRE(assign->op == Assignment::Op::WeakSet);
        REQUIRE(assign->value.as_literal() == "fallback");
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

TEST_CASE("Parser order-only group input", "[parser]")
{
    auto parser = Parser{": foo.c | <gen-headers> |> gcc -c %f -o %o |> foo.o", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->order_only_inputs.size() == 1);
    REQUIRE(rule->order_only_inputs[0].is_order_only_group);
    REQUIRE(rule->order_only_inputs[0].group_name == "gen-headers");
}

TEST_CASE("Parser order-only output group", "[parser]")
{
    auto parser = Parser{": gen.sh |> ./gen.sh |> header.h <gen-headers>", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE(rule->outputs.size() == 1);
    REQUIRE(rule->output_order_only_group.has_value());
    REQUIRE(*rule->output_order_only_group == "gen-headers");
}

TEST_CASE("Parser rule with both group types", "[parser]")
{
    auto parser = Parser{": foreach *.c |> gcc -c %f -o %o |> %B.o {objs} <compiled>", "test.tup"};
    auto result = parser.parse();

    REQUIRE(result.has_value());
    auto const* rule = result->statements[0]->as<Rule>();
    REQUIRE(rule->output_group.has_value());
    REQUIRE(*rule->output_group == "objs");
    REQUIRE(rule->output_order_only_group.has_value());
    REQUIRE(*rule->output_order_only_group == "compiled");
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

    auto parser = Parser { source, "test.tup" };
    auto result = parser.parse();

    REQUIRE(result.has_value());
    REQUIRE(result->statements.size() == 5);
    REQUIRE(result->statements[0]->is<Assignment>());
    REQUIRE(result->statements[1]->is<Assignment>());
    REQUIRE(result->statements[2]->is<BangMacro>());
    REQUIRE(result->statements[3]->is<Rule>());
    REQUIRE(result->statements[4]->is<Rule>());
}

TEST_CASE("Parser nested conditionals", "[parser]")
{
    auto source = R"(
DEBUG = y
VERBOSE = y

ifdef DEBUG
    CFLAGS = -g
    ifdef VERBOSE
        CFLAGS += -DVERBOSE
    endif
endif
)";

    auto parser = Parser { source, "test.tup" };
    auto result = parser.parse();

    REQUIRE(result.has_value());
    // Should have: DEBUG assignment, VERBOSE assignment, outer ifdef (containing nested)
    REQUIRE(result->statements.size() == 3);

    // Check that the outer conditional contains nested statements
    auto const* outer_cond = result->statements[2]->as<Conditional>();
    REQUIRE(outer_cond != nullptr);
    REQUIRE(outer_cond->kind == Conditional::Kind::Ifdef);
    REQUIRE(outer_cond->var_name == "DEBUG");
    // Then body should have: CFLAGS assignment, nested ifdef
    REQUIRE(outer_cond->then_body.size() == 2);
    REQUIRE(outer_cond->then_body[0]->is<Assignment>());
    REQUIRE(outer_cond->then_body[1]->is<Conditional>());

    // Check the nested conditional
    auto const* inner_cond = outer_cond->then_body[1]->as<Conditional>();
    REQUIRE(inner_cond != nullptr);
    REQUIRE(inner_cond->kind == Conditional::Kind::Ifdef);
    REQUIRE(inner_cond->var_name == "VERBOSE");
    REQUIRE(inner_cond->then_body.size() == 1);
    REQUIRE(inner_cond->then_body[0]->is<Assignment>());
}

TEST_CASE("Parser error recovery", "[parser][error]")
{
    SECTION("unterminated variable reference")
    {
        auto parser = Parser { "FOO = $(BAR", "test.tup" };
        auto result = parser.parse();

        // Should fail with error
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("missing endif")
    {
        auto parser = Parser { "ifdef FOO\nBAR = 1\n", "test.tup" };
        auto result = parser.parse();

        // Should fail with missing endif
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("unexpected else")
    {
        auto parser = Parser { "else\n", "test.tup" };
        auto result = parser.parse();

        // Should fail with unexpected else
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("unexpected endif")
    {
        auto parser = Parser { "endif\n", "test.tup" };
        auto result = parser.parse();

        REQUIRE_FALSE(result.has_value());
    }

    SECTION("unterminated foreach")
    {
        auto parser = Parser { "foreach VAR in a b c\nFOO = 1\n", "test.tup" };
        auto result = parser.parse();

        REQUIRE_FALSE(result.has_value());
    }

    SECTION("unclosed at-variable reference")
    {
        auto parser = Parser { "FOO = @(BAR", "test.tup" };
        auto result = parser.parse();

        REQUIRE_FALSE(result.has_value());
    }

    SECTION("rule missing outputs")
    {
        auto parser = Parser { ": foo.c |> gcc -c %f |>", "test.tup" };
        auto result = parser.parse();

        // Rules can have empty outputs (for side-effect only commands)
        // This may or may not be an error depending on implementation
        // Just verify it parses without crashing
        (void)result;
    }
}
