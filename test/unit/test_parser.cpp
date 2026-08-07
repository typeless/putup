// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/parser.hpp"

#include <memory>
#include <string>
#include <variant>
#include <vector>

using namespace pup::parser;
using pup::Vec;
using pup::StringId;
using pup::global_pool;

namespace {
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }
} // namespace

TEST_CASE("Parser empty file", "[parser]")
{
    auto result = parse_tupfile("", "test.tup");

    REQUIRE(result.success());
    REQUIRE(result.tupfile.statements.empty());
}

TEST_CASE("Parser comments and blank lines", "[parser]")
{
    auto result = parse_tupfile("# comment\n\n# another\n", "test.tup");

    REQUIRE(result.success());
    REQUIRE(result.tupfile.statements.empty());
}

TEST_CASE("Parser simple assignment", "[parser]")
{
    SECTION("set")
    {
        auto result = parse_tupfile("FOO = bar", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);
        REQUIRE(result.tupfile.statements[0]->is<Assignment>());

        auto const* assign = result.tupfile.statements[0]->as<Assignment>();
        REQUIRE(assign->name.as_literal() == "FOO");
        REQUIRE(assign->op == Assignment::Op::Set);
        REQUIRE(assign->value.is_literal());
        REQUIRE(assign->value.as_literal() == "bar");
    }

    SECTION("append")
    {
        auto result = parse_tupfile("CFLAGS += -Wall", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);

        auto const* assign = result.tupfile.statements[0]->as<Assignment>();
        REQUIRE(assign->name.as_literal() == "CFLAGS");
        REQUIRE(assign->op == Assignment::Op::Append);
        REQUIRE(assign->value.as_literal() == "-Wall");
    }

    SECTION("define (no expansion)")
    {
        auto result = parse_tupfile("CC := gcc", "test.tup");

        REQUIRE(result.success());
        auto const* assign = result.tupfile.statements[0]->as<Assignment>();
        REQUIRE(assign->op == Assignment::Op::Define);
    }

    SECTION("soft set (?=)")
    {
        auto result = parse_tupfile("FOO ?= default", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);

        auto const* assign = result.tupfile.statements[0]->as<Assignment>();
        REQUIRE(assign->name.as_literal() == "FOO");
        REQUIRE(assign->op == Assignment::Op::SoftSet);
        REQUIRE(assign->value.as_literal() == "default");
    }

    SECTION("weak set (?\?=)")
    {
        // Use ?\?= to avoid trigraph interpretation (??= -> #)
        auto result = parse_tupfile("BAR ?\?= fallback", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);

        auto const* assign = result.tupfile.statements[0]->as<Assignment>();
        REQUIRE(assign->name.as_literal() == "BAR");
        REQUIRE(assign->op == Assignment::Op::WeakSet);
        REQUIRE(assign->value.as_literal() == "fallback");
    }
}

TEST_CASE("Parser variable references in assignment", "[parser]")
{
    auto result = parse_tupfile("LDFLAGS = $(CFLAGS) -lm", "test.tup");

    REQUIRE(result.success());
    auto const* assign = result.tupfile.statements[0]->as<Assignment>();
    REQUIRE(assign->value.parts.size() == 2);

    // First part is variable reference
    REQUIRE(std::holds_alternative<Expression::Variable>(assign->value.parts[0]));
    auto const& var = std::get<Expression::Variable>(assign->value.parts[0]);
    REQUIRE(var.ref.kind == VarRef::Kind::Regular);
    REQUIRE(sv(var.ref.name) == "CFLAGS");

    // Second part is literal
    REQUIRE(std::holds_alternative<Expression::Literal>(assign->value.parts[1]));
}

TEST_CASE("Parser include directives", "[parser]")
{
    SECTION("include_rules")
    {
        auto result = parse_tupfile("include_rules", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);
        REQUIRE(result.tupfile.statements[0]->is<Include>());

        auto const* inc = result.tupfile.statements[0]->as<Include>();
        REQUIRE(inc->is_rules);
    }

    SECTION("include path")
    {
        auto result = parse_tupfile("include foo/bar.tup", "test.tup");

        REQUIRE(result.success());
        auto const* inc = result.tupfile.statements[0]->as<Include>();
        REQUIRE_FALSE(inc->is_rules);
        REQUIRE(inc->path.as_literal() == "foo/bar.tup");
    }
}

TEST_CASE("Parser export/import", "[parser]")
{
    SECTION("export")
    {
        auto result = parse_tupfile("export PATH", "test.tup");

        REQUIRE(result.success());
        auto const* exp = result.tupfile.statements[0]->as<Export>();
        REQUIRE(sv(exp->var_name) == "PATH");
    }

    SECTION("import without default")
    {
        auto result = parse_tupfile("import CC", "test.tup");

        REQUIRE(result.success());
        auto const* imp = result.tupfile.statements[0]->as<Import>();
        REQUIRE(sv(imp->var_name) == "CC");
        REQUIRE_FALSE(imp->default_value.has_value());
    }

    SECTION("import with default")
    {
        auto result = parse_tupfile("import CC=gcc", "test.tup");

        REQUIRE(result.success());
        auto const* imp = result.tupfile.statements[0]->as<Import>();
        REQUIRE(sv(imp->var_name) == "CC");
        REQUIRE(imp->default_value.has_value());
        REQUIRE(imp->default_value->as_literal() == "gcc");
    }
}

TEST_CASE("Parser error directive", "[parser]")
{
    SECTION("error with message")
    {
        auto result = parse_tupfile("error message here", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);
        REQUIRE(result.tupfile.statements[0]->is<ErrorDirective>());

        auto const* err = result.tupfile.statements[0]->as<ErrorDirective>();
        REQUIRE(err->message.as_literal() == "message here");
    }

    SECTION("bare error")
    {
        auto result = parse_tupfile("error", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);
        REQUIRE(result.tupfile.statements[0]->is<ErrorDirective>());

        auto const* err = result.tupfile.statements[0]->as<ErrorDirective>();
        REQUIRE(err->message.empty());
    }

    SECTION("error followed by assignment operator stays an assignment")
    {
        auto result = parse_tupfile("error = value", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);
        REQUIRE(result.tupfile.statements[0]->is<Assignment>());

        auto const* assign = result.tupfile.statements[0]->as<Assignment>();
        REQUIRE(assign->name.as_literal() == "error");
        REQUIRE(assign->value.as_literal() == "value");
    }

    SECTION("error message with variable reference")
    {
        auto result = parse_tupfile("error $(V) tail", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements[0]->is<ErrorDirective>());

        auto const* err = result.tupfile.statements[0]->as<ErrorDirective>();
        REQUIRE(err->message.parts.size() == 2);
        REQUIRE(std::holds_alternative<Expression::Variable>(err->message.parts[0]));
        auto const& var = std::get<Expression::Variable>(err->message.parts[0]);
        REQUIRE(var.ref.kind == VarRef::Kind::Regular);
        REQUIRE(sv(var.ref.name) == "V");
    }

    SECTION("compound assignment LHS starting with error stays an assignment")
    {
        auto result = parse_tupfile("error$(SUF) = value", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);
        REQUIRE(result.tupfile.statements[0]->is<Assignment>());

        auto const* assign = result.tupfile.statements[0]->as<Assignment>();
        REQUIRE(assign->name.parts.size() == 2);
        REQUIRE(assign->value.as_literal() == "value");
    }

    SECTION("compound non-assignment line starting with error is skipped")
    {
        auto result = parse_tupfile("error$(SUF) tail", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.empty());
    }
}

TEST_CASE("Parser conditionals", "[parser]")
{
    SECTION("ifdef")
    {
        auto result = parse_tupfile("ifdef DEBUG\nCFLAGS += -g\nendif", "test.tup");

        REQUIRE(result.success());
        REQUIRE(result.tupfile.statements.size() == 1);
        REQUIRE(result.tupfile.statements[0]->is<Conditional>());

        auto const* cond = result.tupfile.statements[0]->as<Conditional>();
        REQUIRE(cond->kind == Conditional::Kind::Ifdef);
        REQUIRE(sv(cond->var_name) == "DEBUG");
        REQUIRE(cond->then_body.size() == 1);
        REQUIRE(cond->else_body.empty());
    }

    SECTION("ifdef with else")
    {
        auto result = parse_tupfile("ifdef DEBUG\nCFLAGS += -g\nelse\nCFLAGS += -O2\nendif", "test.tup");

        REQUIRE(result.success());
        auto const* cond = result.tupfile.statements[0]->as<Conditional>();
        REQUIRE(cond->then_body.size() == 1);
        REQUIRE(cond->else_body.size() == 1);
    }

    SECTION("ifeq")
    {
        auto result = parse_tupfile("ifeq ($(CC),gcc)\nCFLAGS += -Wall\nendif", "test.tup");

        REQUIRE(result.success());
        auto const* cond = result.tupfile.statements[0]->as<Conditional>();
        REQUIRE(cond->kind == Conditional::Kind::Ifeq);
        REQUIRE(cond->then_body.size() == 1);
    }
}

TEST_CASE("Parser simple rule", "[parser]")
{
    auto result = parse_tupfile(": foo.c |> gcc -c %f -o %o |> foo.o", "test.tup");

    REQUIRE(result.success());
    REQUIRE(result.tupfile.statements.size() == 1);
    REQUIRE(result.tupfile.statements[0]->is<Rule>());

    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE_FALSE(rule->foreach_);
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->inputs[0].path.as_literal() == "foo.c");
    REQUIRE(rule->order_only_inputs.empty());
    REQUIRE(rule->outputs.size() == 1);
    REQUIRE(rule->outputs[0].path.as_literal() == "foo.o");
}

TEST_CASE("Parser foreach rule", "[parser]")
{
    auto result = parse_tupfile(": foreach *.c |> gcc -c %f -o %o |> %B.o", "test.tup");

    REQUIRE(result.success());
    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE(rule->foreach_);
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->inputs[0].path.as_literal() == "*.c");
}

TEST_CASE("Parser rule with order-only inputs", "[parser]")
{
    auto result = parse_tupfile(": foo.c | bar.h |> gcc -c %f -o %o |> foo.o", "test.tup");

    REQUIRE(result.success());
    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->order_only_inputs.size() == 1);
    REQUIRE(rule->order_only_inputs[0].path.as_literal() == "bar.h");
}

TEST_CASE("Parser rule with output group", "[parser]")
{
    auto result = parse_tupfile(": foreach *.c |> gcc -c %f -o %o |> %B.o {objs}", "test.tup");

    REQUIRE(result.success());
    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE(rule->output_group.has_value());
    REQUIRE(sv(*rule->output_group) == "objs");
}

TEST_CASE("Parser rule with group input", "[parser]")
{
    auto result = parse_tupfile(": {objs} |> ar rcs %o %f |> libfoo.a", "test.tup");

    REQUIRE(result.success());
    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->inputs[0].is_group);
    REQUIRE(sv(rule->inputs[0].group_name) == "objs");
}

TEST_CASE("Parser order-only group input", "[parser]")
{
    auto result = parse_tupfile(": foo.c | <gen-headers> |> gcc -c %f -o %o |> foo.o", "test.tup");

    REQUIRE(result.success());
    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE(rule->inputs.size() == 1);
    REQUIRE(rule->order_only_inputs.size() == 1);
    REQUIRE(rule->order_only_inputs[0].is_order_only_group);
    REQUIRE(sv(rule->order_only_inputs[0].group_name) == "gen-headers");
}

TEST_CASE("Parser order-only output group", "[parser]")
{
    auto result = parse_tupfile(": gen.sh |> ./gen.sh |> header.h <gen-headers>", "test.tup");

    REQUIRE(result.success());
    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE(rule->outputs.size() == 1);
    REQUIRE(rule->output_order_only_group.has_value());
    REQUIRE(sv(*rule->output_order_only_group) == "gen-headers");
}

TEST_CASE("Parser rule with both group types", "[parser]")
{
    auto result = parse_tupfile(": foreach *.c |> gcc -c %f -o %o |> %B.o {objs} <compiled>", "test.tup");

    REQUIRE(result.success());
    auto const* rule = result.tupfile.statements[0]->as<Rule>();
    REQUIRE(rule->output_group.has_value());
    REQUIRE(sv(*rule->output_group) == "objs");
    REQUIRE(rule->output_order_only_group.has_value());
    REQUIRE(sv(*rule->output_order_only_group) == "compiled");
}

TEST_CASE("Parser bang macro", "[parser]")
{
    auto result = parse_tupfile("!cc = |> gcc -c %f -o %o |> %B.o", "test.tup");

    REQUIRE(result.success());
    REQUIRE(result.tupfile.statements.size() == 1);
    REQUIRE(result.tupfile.statements[0]->is<BangMacro>());

    auto const* macro = result.tupfile.statements[0]->as<BangMacro>();
    REQUIRE(sv(macro->name) == "cc");
    REQUIRE(macro->outputs.size() == 1);
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

    auto result = parse_tupfile(source, "test.tup");

    REQUIRE(result.success());
    REQUIRE(result.tupfile.statements.size() == 5);
    REQUIRE(result.tupfile.statements[0]->is<Assignment>());
    REQUIRE(result.tupfile.statements[1]->is<Assignment>());
    REQUIRE(result.tupfile.statements[2]->is<BangMacro>());
    REQUIRE(result.tupfile.statements[3]->is<Rule>());
    REQUIRE(result.tupfile.statements[4]->is<Rule>());
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

    auto result = parse_tupfile(source, "test.tup");

    REQUIRE(result.success());
    // Should have: DEBUG assignment, VERBOSE assignment, outer ifdef (containing nested)
    REQUIRE(result.tupfile.statements.size() == 3);

    // Check that the outer conditional contains nested statements
    auto const* outer_cond = result.tupfile.statements[2]->as<Conditional>();
    REQUIRE(outer_cond != nullptr);
    REQUIRE(outer_cond->kind == Conditional::Kind::Ifdef);
    REQUIRE(sv(outer_cond->var_name) == "DEBUG");
    // Then body should have: CFLAGS assignment, nested ifdef
    REQUIRE(outer_cond->then_body.size() == 2);
    REQUIRE(outer_cond->then_body[0]->is<Assignment>());
    REQUIRE(outer_cond->then_body[1]->is<Conditional>());

    // Check the nested conditional
    auto const* inner_cond = outer_cond->then_body[1]->as<Conditional>();
    REQUIRE(inner_cond != nullptr);
    REQUIRE(inner_cond->kind == Conditional::Kind::Ifdef);
    REQUIRE(sv(inner_cond->var_name) == "VERBOSE");
    REQUIRE(inner_cond->then_body.size() == 1);
    REQUIRE(inner_cond->then_body[0]->is<Assignment>());
}

TEST_CASE("Parser error recovery", "[parser][error]")
{
    SECTION("unterminated variable reference")
    {
        auto result = parse_tupfile("FOO = $(BAR", "test.tup");

        // Should fail with error
        REQUIRE_FALSE(result.success());
    }

    SECTION("missing endif")
    {
        auto result = parse_tupfile("ifdef FOO\nBAR = 1\n", "test.tup");

        // Should fail with missing endif
        REQUIRE_FALSE(result.success());
    }

    SECTION("unexpected else")
    {
        auto result = parse_tupfile("else\n", "test.tup");

        // Should fail with unexpected else
        REQUIRE_FALSE(result.success());
    }

    SECTION("unexpected endif")
    {
        auto result = parse_tupfile("endif\n", "test.tup");

        REQUIRE_FALSE(result.success());
    }

    SECTION("unterminated foreach")
    {
        auto result = parse_tupfile("foreach VAR in a b c\nFOO = 1\n", "test.tup");

        REQUIRE_FALSE(result.success());
    }

    SECTION("unclosed at-variable reference")
    {
        auto result = parse_tupfile("FOO = @(BAR", "test.tup");

        REQUIRE_FALSE(result.success());
    }

    SECTION("rule missing outputs")
    {
        auto result = parse_tupfile(": foo.c |> gcc -c %f |>", "test.tup");

        // Rules can have empty outputs (for side-effect only commands)
        // This may or may not be an error depending on implementation
        // Just verify it parses without crashing
        (void)result;
    }
}

namespace {

auto render(Expression const& expr) -> std::string
{
    auto out = std::string {};
    for (auto const& part : expr.parts) {
        if (auto const* literal = std::get_if<Expression::Literal>(&part)) {
            out += global_pool().get(literal->value);
        } else {
            out += "$(";
            out += global_pool().get(std::get<Expression::Variable>(part).ref.name);
            out += ")";
        }
    }
    return out;
}

auto render(Vec<PathPattern> const& patterns) -> std::string
{
    auto out = std::string {};
    for (auto const& pattern : patterns) {
        out += render(pattern.path);
        out += " ";
    }
    return out;
}

auto render(Vec<std::unique_ptr<Statement>> const& statements) -> std::string;

auto render(Statement const& statement) -> std::string
{
    if (auto const* assign = statement.as<Assignment>()) {
        return render(assign->name) + "=" + render(assign->value) + ";";
    }
    if (auto const* rule = statement.as<Rule>()) {
        return render(rule->inputs) + "|>" + render(rule->command) + "|>" + render(rule->outputs) + ";";
    }
    if (auto const* cond = statement.as<Conditional>()) {
        return "if(" + render(cond->lhs) + "," + render(cond->rhs) + "){" + render(cond->then_body) + "}else{"
            + render(cond->else_body) + "};";
    }
    if (auto const* include = statement.as<Include>()) {
        return "include(" + render(include->path) + ");";
    }
    return "?;";
}

auto render(Vec<std::unique_ptr<Statement>> const& statements) -> std::string
{
    auto out = std::string {};
    for (auto const& statement : statements) {
        out += render(*statement);
    }
    return out;
}

auto render(Tupfile const& tupfile) -> std::string
{
    return render(tupfile.statements);
}

auto replace_at(std::string_view text, std::size_t pos, std::size_t len, std::string_view with) -> std::string
{
    return std::string { text.substr(0, pos) } + std::string { with } + std::string { text.substr(pos + len) };
}

} // namespace

TEST_CASE("Parser renders a continuation as spaces", "[parser][continuation]")
{
    SECTION("rule command is single-line")
    {
        auto continued = parse_tupfile(": foo.c |> gcc -c \\\nfoo.c |> foo.o", "test.tup");
        auto spaced = parse_tupfile(": foo.c |> gcc -c   foo.c |> foo.o", "test.tup");

        REQUIRE(continued.success());
        REQUIRE(render(continued.tupfile).find('\n') == std::string::npos);
        REQUIRE(render(continued.tupfile) == render(spaced.tupfile));
    }

    SECTION("CRLF continuation parses")
    {
        auto continued = parse_tupfile(": foo.c |> gcc -c \\\r\nfoo.c |> foo.o\r\n", "test.tup");
        auto spaced = parse_tupfile(": foo.c |> gcc -c    foo.c |> foo.o\r\n", "test.tup");

        REQUIRE(continued.success());
        REQUIRE(render(continued.tupfile) == render(spaced.tupfile));
    }

    SECTION("variable value carries no newline")
    {
        auto continued = parse_tupfile("FLAGS = -DA\\\n-DB\n", "test.tup");
        auto spaced = parse_tupfile("FLAGS = -DA  -DB\n", "test.tup");

        REQUIRE(continued.success());
        REQUIRE(render(continued.tupfile).find('\n') == std::string::npos);
        REQUIRE(render(continued.tupfile) == render(spaced.tupfile));
    }
}

TEST_CASE("Parser continuation equals its spelling in spaces at every position", "[parser][continuation]")
{
    auto const bases = std::vector<std::string> {
        "FLAGS = -DA  -DB  -DC\n",
        ": foo.c  bar.c |> gcc  -c  %f  -o  %o |> %B.o\n",
        "FLAGS = -DA\n: foo.c |> gcc  $(FLAGS)  %f  -o  %o |> %B.o\n",
        ": foo.c |> gcc -c %f -o %o |> obj/  %B.o\n",
    };

    for (auto const& base : bases) {
        auto const base_result = parse_tupfile(base, "test.tup");
        INFO("base: " << base);
        REQUIRE(base_result.success());

        auto const expected = render(base_result.tupfile);
        REQUIRE_FALSE(expected.empty());

        for (auto pos = base.find("  "); pos != std::string::npos; pos = base.find("  ", pos + 1)) {
            INFO("position: " << pos);

            auto const lf = replace_at(base, pos, 2, "\\\n");
            auto const lf_result = parse_tupfile(lf, "test.tup");
            REQUIRE(lf_result.success());
            REQUIRE(render(lf_result.tupfile) == expected);

            auto const crlf = replace_at(base, pos, 2, "\\\r\n");
            auto const crlf_result = parse_tupfile(crlf, "test.tup");
            REQUIRE(crlf_result.success());
            REQUIRE(render(crlf_result.tupfile) == render(parse_tupfile(replace_at(base, pos, 2, "   "), "test.tup").tupfile));
        }
    }
}

TEST_CASE("Parser ignores whitespace at the end of a line", "[parser][continuation]")
{
    // The #349 rewrite spells a line-ending carriage return as a space, so every context has to read a trailing whitespace run as nothing at all.
    auto const bases = std::vector<std::string> {
        "X = $(Y)\n",
        "Y = v\nX = $(Y)\n: foo.c |> echo $(X) > %o |> out/%B.o\n",
        "X = y\nifeq ($(X),y)\n: foo.c |> echo yes > %o |> %B.o\nelse\n: foo.c |> echo no > %o |> %B.o\nendif\n",
        ": foo.c bar.c |> gcc -c %f -o %o |> obj/%B.o\n",
        ": foo.c |> gcc -c \\\n    -Iinc \\\n    %f -o %o |> %B.o\n",
        "!cc = |> gcc -c %f -o %o |>\n: foo.c |> !cc |> %B.o\n",
        "include extra.tup\n",
    };
    auto const runs = std::vector<std::string> { " ", "\t", "  ", " \t ", "\t\t  ", "\r", " \r", "\t\r", "\r\r " };

    auto cases = 0;
    for (auto const& base : bases) {
        auto const expected = parse_tupfile(base, "test.tup");
        INFO("base: " << base);
        REQUIRE(expected.success());
        REQUIRE_FALSE(render(expected.tupfile).empty());

        for (auto const& run : runs) {
            // A backslash's adjacency to the newline is what makes it a continuation, in tup too, so that position is outside this law rather than an exception to it.
            auto padded = std::string {};
            for (auto const c : base) {
                if (c == '\n' && !padded.empty() && padded.back() != '\\') {
                    padded += run;
                }
                padded += c;
            }
            INFO("run: [" << run << "]");

            auto const actual = parse_tupfile(padded, "test.tup");
            REQUIRE(actual.success());
            REQUIRE(render(actual.tupfile) == render(expected.tupfile));
            ++cases;
        }
    }
    REQUIRE(cases == static_cast<int>(bases.size() * runs.size()));
}
