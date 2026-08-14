// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "temp_root.hpp"

#include "pup/cli/strict_checks.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

using namespace pup::cli;
using pup::StringId;
using pup::global_pool;
using pup::parser::Assignment;
using pup::parser::Expression;
using pup::parser::VarRef;

namespace {

auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }

auto make_literal_expr(std::string_view text) -> Expression
{
    auto expr = Expression {};
    expr.parts.push_back(Expression::Literal { intern(text) });
    return expr;
}

auto make_var_expr(std::string_view var_name) -> Expression
{
    auto expr = Expression {};
    expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern(var_name), {} } });
    return expr;
}

auto make_assignment(std::string_view name, Assignment::Op op, Expression value) -> Assignment
{
    auto stmt = Assignment {};
    stmt.name = make_literal_expr(name);
    stmt.op = op;
    stmt.value = std::move(value);
    stmt.location.line = 1;
    return stmt;
}

} // namespace

TEST_CASE("check_assignment: S anchor variable", "[strict]")
{
    SECTION("S ?= $(TUP_CWD) in component — no diagnostic")
    {
        auto stmt = make_assignment("S", Assignment::Op::SoftSet, make_var_expr("TUP_CWD"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }

    SECTION("S = $(TUP_CWD) in component — error")
    {
        auto stmt = make_assignment("S", Assignment::Op::Set, make_var_expr("TUP_CWD"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Error);
        REQUIRE(sv(diags[0].message).find("must use '?='") != std::string_view::npos);
    }

    SECTION("S = $(TUP_CWD) in root — no diagnostic (exempt)")
    {
        auto stmt = make_assignment("S", Assignment::Op::Set, make_var_expr("TUP_CWD"));
        auto diags = check_assignment(stmt, "Tuprules.tup", false);
        REQUIRE(diags.empty());
    }

    SECTION("S ?= literal in component — warning")
    {
        auto stmt = make_assignment("S", Assignment::Op::SoftSet, make_literal_expr(".."));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Warning);
    }
}

TEST_CASE("check_assignment: B anchor variable", "[strict]")
{
    SECTION("B ?= with $(S) in component — no diagnostic")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("TUP_VARIANT_OUTPUTDIR"), {} } });
        expr.parts.push_back(Expression::Literal { intern("/") });
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("S"), {} } });

        auto stmt = make_assignment("B", Assignment::Op::SoftSet, std::move(expr));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }

    SECTION("B = ... in component — error")
    {
        auto stmt = make_assignment("B", Assignment::Op::Set, make_literal_expr("build"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Error);
    }
}

TEST_CASE("check_assignment: toolchain variables", "[strict]")
{
    SECTION("CC ?= gcc in component — no diagnostic")
    {
        auto stmt = make_assignment("CC", Assignment::Op::SoftSet, make_literal_expr("gcc"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }

    SECTION("CC = gcc in component — warning")
    {
        auto stmt = make_assignment("CC", Assignment::Op::Set, make_literal_expr("gcc"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Warning);
    }

    SECTION("CC = gcc in root — no diagnostic")
    {
        auto stmt = make_assignment("CC", Assignment::Op::Set, make_literal_expr("gcc"));
        auto diags = check_assignment(stmt, "Tuprules.tup", false);
        REQUIRE(diags.empty());
    }

    SECTION("CFLAGS = -O2 in component — no diagnostic (not toolchain)")
    {
        auto stmt = make_assignment("CFLAGS", Assignment::Op::Set, make_literal_expr("-O2"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }
}

TEST_CASE("check_assignment: non-Tuprules file — no diagnostic", "[strict]")
{
    auto stmt = make_assignment("S", Assignment::Op::Set, make_var_expr("TUP_CWD"));
    auto diags = check_assignment(stmt, "libfoo/Tupfile", true);
    REQUIRE(diags.empty());
}

TEST_CASE("check_component_dirs: missing Tupfile.ini — warning", "[strict]")
{
    auto const missing = pup::test::temp_path("nonexistent_dir_for_test").string();
    auto dirs = pup::Vec<std::string_view> { missing };
    auto diags = check_component_dirs(dirs);
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].severity == Diagnostic::Warning);
    REQUIRE(sv(diags[0].message).find("no Tupfile.ini") != std::string_view::npos);
}

TEST_CASE("check_component_dirs: empty list — no diagnostic", "[strict]")
{
    auto dirs = pup::Vec<std::string_view> {};
    auto diags = check_component_dirs(dirs);
    REQUIRE(diags.empty());
}
