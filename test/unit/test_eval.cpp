// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/parser/eval.hpp"

using namespace pup::parser;

TEST_CASE("VarDb basic operations", "[eval]")
{
    auto db = VarDb{};

    SECTION("set and get")
    {
        db.set("FOO", "bar");
        REQUIRE(db.get("FOO") == "bar");
        REQUIRE(db.contains("FOO"));
    }

    SECTION("get nonexistent returns empty")
    {
        REQUIRE(db.get("NONEXISTENT").empty());
        REQUIRE_FALSE(db.contains("NONEXISTENT"));
    }

    SECTION("append")
    {
        db.set("FLAGS", "-Wall");
        db.append("FLAGS", "-Wextra");
        REQUIRE(db.get("FLAGS") == "-Wall -Wextra");
    }

    SECTION("append to nonexistent")
    {
        db.append("NEW", "value");
        REQUIRE(db.get("NEW") == "value");
    }

    SECTION("remove")
    {
        db.set("FOO", "bar");
        db.remove("FOO");
        REQUIRE_FALSE(db.contains("FOO"));
    }

    SECTION("clear")
    {
        db.set("A", "1");
        db.set("B", "2");
        db.clear();
        REQUIRE_FALSE(db.contains("A"));
        REQUIRE_FALSE(db.contains("B"));
    }
}

TEST_CASE("Evaluator expression expansion", "[eval]")
{
    auto vars = VarDb{};
    auto ctx = EvalContext{.vars = &vars};
    auto eval = Evaluator{ctx};

    SECTION("literal expression")
    {
        auto expr = Expression{};
        expr.parts.push_back(Expression::Literal{"hello world"});

        auto result = eval.expand(expr);
        REQUIRE(result.has_value());
        REQUIRE(*result == "hello world");
    }

    SECTION("variable expansion")
    {
        vars.set("NAME", "pup");

        auto expr = Expression{};
        expr.parts.push_back(Expression::Literal{"hello "});
        expr.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "NAME", {}}});

        auto result = eval.expand(expr);
        REQUIRE(result.has_value());
        REQUIRE(*result == "hello pup");
    }

    SECTION("multiple variables")
    {
        vars.set("CC", "gcc");
        vars.set("CFLAGS", "-Wall -O2");

        auto expr = Expression{};
        expr.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "CC", {}}});
        expr.parts.push_back(Expression::Literal{" "});
        expr.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "CFLAGS", {}}});

        auto result = eval.expand(expr);
        REQUIRE(result.has_value());
        REQUIRE(*result == "gcc -Wall -O2");
    }

    SECTION("undefined variable expands to empty")
    {
        auto expr = Expression{};
        expr.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "UNDEFINED", {}}});

        auto result = eval.expand(expr);
        REQUIRE(result.has_value());
        REQUIRE(result->empty());
    }
}

TEST_CASE("Evaluator string expansion", "[eval]")
{
    auto vars = VarDb{};
    auto ctx = EvalContext{.vars = &vars};
    auto eval = Evaluator{ctx};

    SECTION("no variables")
    {
        auto result = eval.expand("hello world");
        REQUIRE(result.has_value());
        REQUIRE(*result == "hello world");
    }

    SECTION("single variable")
    {
        vars.set("NAME", "pup");
        auto result = eval.expand("hello $(NAME)");
        REQUIRE(result.has_value());
        REQUIRE(*result == "hello pup");
    }

    SECTION("multiple variables")
    {
        vars.set("CC", "gcc");
        vars.set("FLAGS", "-O2");
        auto result = eval.expand("$(CC) $(FLAGS)");
        REQUIRE(result.has_value());
        REQUIRE(*result == "gcc -O2");
    }

    SECTION("dollar without paren is literal")
    {
        auto result = eval.expand("price is $5");
        REQUIRE(result.has_value());
        REQUIRE(*result == "price is $5");
    }
}

TEST_CASE("Evaluator config variables", "[eval]")
{
    auto vars = VarDb{};
    auto config_vars = VarDb{};
    auto ctx = EvalContext{.vars = &vars, .config_vars = &config_vars};
    auto eval = Evaluator{ctx};

    SECTION("config variable expansion")
    {
        config_vars.set("DEBUG", "y");

        auto expr = Expression{};
        expr.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Config, "DEBUG", {}}});

        auto result = eval.expand(expr);
        REQUIRE(result.has_value());
        REQUIRE(*result == "y");
    }
}

TEST_CASE("Evaluator built-in variables", "[eval]")
{
    auto vars = VarDb{};
    auto ctx = EvalContext{
        .vars = &vars,
        .tup_cwd = "/home/user/project/src",
        .tup_platform = "linux",
        .tup_arch = "x86_64",
    };
    auto eval = Evaluator{ctx};

    SECTION("TUP_CWD")
    {
        auto expr = Expression{};
        expr.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "TUP_CWD", {}}});

        auto result = eval.expand(expr);
        REQUIRE(result.has_value());
        REQUIRE(*result == "/home/user/project/src");
    }

    SECTION("TUP_PLATFORM")
    {
        auto expr = Expression{};
        expr.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "TUP_PLATFORM", {}}});

        auto result = eval.expand(expr);
        REQUIRE(result.has_value());
        REQUIRE(*result == "linux");
    }
}

TEST_CASE("Evaluator pattern expansion", "[eval]")
{
    auto vars = VarDb{};
    auto ctx = EvalContext{.vars = &vars};
    auto eval = Evaluator{ctx};

    // For foreach rules, all_inputs has just one element (the current input)
    auto flags = PatternFlags{
        .input = "src/foo.c",
        .input_base = "foo.c",
        .input_noext = "foo",
        .input_ext = "c",
        .output = "build/foo.o",
        .output_base = "foo.o",
        .input_dir = "src",
        .all_inputs = {"src/foo.c"},
    };

    SECTION("%f - input filename")
    {
        auto result = eval.expand_pattern("gcc -c %f -o %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(*result == "gcc -c src/foo.c -o build/foo.o");
    }

    SECTION("%B - basename without extension")
    {
        auto result = eval.expand_pattern("%B.o", flags);
        REQUIRE(result.has_value());
        REQUIRE(*result == "foo.o");
    }

    SECTION("%% escape")
    {
        auto result = eval.expand_pattern("100%%", flags);
        REQUIRE(result.has_value());
        REQUIRE(*result == "100%");
    }

    SECTION("%Nf - N-th input (single)")
    {
        auto result = eval.expand_pattern("%1f", flags);
        REQUIRE(result.has_value());
        REQUIRE(*result == "src/foo.c");
    }

    // Note: %i is for order-only inputs, not yet implemented
}

TEST_CASE("Evaluator pattern expansion - multiple inputs", "[eval]")
{
    auto vars = VarDb{};
    auto ctx = EvalContext{.vars = &vars};
    auto eval = Evaluator{ctx};

    // For non-foreach rules, all_inputs has all input files
    auto flags = PatternFlags{
        .input = "a.c",
        .input_base = "a.c",
        .input_noext = "a",
        .input_ext = "c",
        .output = "out.o",
        .output_base = "out.o",
        .input_dir = "",
        .all_inputs = {"a.c", "b.c", "c.c"},
    };

    SECTION("%f - all inputs")
    {
        auto result = eval.expand_pattern("gcc -c %f -o %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(*result == "gcc -c a.c b.c c.c -o out.o");
    }

    SECTION("%Nf - N-th input")
    {
        auto result = eval.expand_pattern("%1f %2f %3f", flags);
        REQUIRE(result.has_value());
        REQUIRE(*result == "a.c b.c c.c");
    }

    // Note: %i is for order-only inputs, not yet implemented
}

TEST_CASE("Evaluator conditionals", "[eval]")
{
    auto vars = VarDb{};
    auto config_vars = VarDb{};
    auto ctx = EvalContext{.vars = &vars, .config_vars = &config_vars};
    auto eval = Evaluator{ctx};

    SECTION("ifdef - true when defined")
    {
        vars.set("DEBUG", "1");

        auto cond = Conditional{};
        cond.kind = Conditional::Kind::Ifdef;
        cond.var_name = "DEBUG";

        REQUIRE(eval.evaluate_condition(cond));
    }

    SECTION("ifdef - false when undefined")
    {
        auto cond = Conditional{};
        cond.kind = Conditional::Kind::Ifdef;
        cond.var_name = "UNDEFINED";

        REQUIRE_FALSE(eval.evaluate_condition(cond));
    }

    SECTION("ifndef - true when undefined")
    {
        auto cond = Conditional{};
        cond.kind = Conditional::Kind::Ifndef;
        cond.var_name = "UNDEFINED";

        REQUIRE(eval.evaluate_condition(cond));
    }

    SECTION("ifeq - true when equal")
    {
        vars.set("CC", "gcc");

        auto cond = Conditional{};
        cond.kind = Conditional::Kind::Ifeq;
        cond.lhs.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "CC", {}}});
        cond.rhs.parts.push_back(Expression::Literal{"gcc"});

        REQUIRE(eval.evaluate_condition(cond));
    }

    SECTION("ifeq - false when not equal")
    {
        vars.set("CC", "clang");

        auto cond = Conditional{};
        cond.kind = Conditional::Kind::Ifeq;
        cond.lhs.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "CC", {}}});
        cond.rhs.parts.push_back(Expression::Literal{"gcc"});

        REQUIRE_FALSE(eval.evaluate_condition(cond));
    }

    SECTION("ifneq - true when not equal")
    {
        vars.set("CC", "clang");

        auto cond = Conditional{};
        cond.kind = Conditional::Kind::Ifneq;
        cond.lhs.parts.push_back(Expression::Variable{VarRef{VarRef::Kind::Regular, "CC", {}}});
        cond.rhs.parts.push_back(Expression::Literal{"gcc"});

        REQUIRE(eval.evaluate_condition(cond));
    }
}
