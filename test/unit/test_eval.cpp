// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/eval.hpp"

using namespace pup::parser;
using pup::StringId;
using pup::global_pool;
using pup::test::EnvGuard;

namespace {
auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }
} // namespace

TEST_CASE("VarDb basic operations", "[eval]")
{
    auto db = VarDb {};

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
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    SECTION("literal expression")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Literal { intern("hello world") });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "hello world");
    }

    SECTION("variable expansion")
    {
        vars.set("NAME", "pup");

        auto expr = Expression {};
        expr.parts.push_back(Expression::Literal { intern("hello ") });
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("NAME"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "hello pup");
    }

    SECTION("multiple variables")
    {
        vars.set("CC", "gcc");
        vars.set("CFLAGS", "-Wall -O2");

        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("CC"), {} } });
        expr.parts.push_back(Expression::Literal { intern(" ") });
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("CFLAGS"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "gcc -Wall -O2");
    }

    SECTION("undefined variable expands to empty")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("UNDEFINED"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(is_empty(*result));
    }
}

TEST_CASE("Evaluator string expansion", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    SECTION("no variables")
    {
        auto result = expand(ctx,"hello world");
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "hello world");
    }

    SECTION("single variable")
    {
        vars.set("NAME", "pup");
        auto result = expand(ctx,"hello $(NAME)");
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "hello pup");
    }

    SECTION("multiple variables")
    {
        vars.set("CC", "gcc");
        vars.set("FLAGS", "-O2");
        auto result = expand(ctx,"$(CC) $(FLAGS)");
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "gcc -O2");
    }

    SECTION("dollar without paren is literal")
    {
        auto result = expand(ctx,"price is $5");
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "price is $5");
    }
}

TEST_CASE("Evaluator config variables", "[eval]")
{
    auto vars = VarDb {};
    auto config_vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars, .config_vars = &config_vars };


    SECTION("config variable expansion")
    {
        config_vars.set("DEBUG", "y");

        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Config, intern("DEBUG"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "y");
    }
}

TEST_CASE("Evaluator built-in variables", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext {
        .vars = &vars,
        .tup_cwd = intern("/home/user/project/src"),
        .tup_platform = intern("linux"),
        .tup_arch = intern("x86_64"),
    };


    SECTION("TUP_CWD")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("TUP_CWD"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "/home/user/project/src");
    }

    SECTION("TUP_PLATFORM")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("TUP_PLATFORM"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "linux");
    }
}

TEST_CASE("get_platform() respects TUP_PLATFORM env var", "[eval][platform]")
{
    SECTION("returns compile-time default when env var not set")
    {
        auto platform = pup::get_platform();
        REQUIRE(sv(platform) == pup::PLATFORM);
    }

    SECTION("returns env var value when TUP_PLATFORM is set")
    {
        auto env = EnvGuard { "TUP_PLATFORM", "win32" };
        auto platform = pup::get_platform();
        REQUIRE(sv(platform) == "win32");
    }

    SECTION("returns env var for custom platform names")
    {
        auto env = EnvGuard { "TUP_PLATFORM", "custom-platform" };
        auto platform = pup::get_platform();
        REQUIRE(sv(platform) == "custom-platform");
    }
}

TEST_CASE("@(TUP_PLATFORM) respects CONFIG_TUP_PLATFORM in tup.config", "[eval][platform]")
{
    auto vars = VarDb {};
    auto config_vars = VarDb {};
    auto ctx = EvalContext {
        .vars = &vars,
        .config_vars = &config_vars,
        .tup_platform = intern(pup::PLATFORM), // compile-time default
    };


    SECTION("returns compile-time default when config not set")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Config, intern("TUP_PLATFORM"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == pup::PLATFORM);
    }

    SECTION("returns config value when CONFIG_TUP_PLATFORM is set")
    {
        config_vars.set("TUP_PLATFORM", "win32");

        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Config, intern("TUP_PLATFORM"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "win32");
    }

    SECTION("env var takes highest priority over config")
    {
        auto env = EnvGuard { "TUP_PLATFORM", "env-platform" };
        config_vars.set("TUP_PLATFORM", "config-platform");

        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Config, intern("TUP_PLATFORM"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "env-platform");
    }
}

TEST_CASE("@(TUP_ARCH) respects CONFIG_TUP_ARCH in tup.config", "[eval][arch]")
{
    auto vars = VarDb {};
    auto config_vars = VarDb {};
    auto ctx = EvalContext {
        .vars = &vars,
        .config_vars = &config_vars,
        .tup_arch = intern(pup::ARCH), // compile-time default
    };


    SECTION("returns compile-time default when config not set")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Config, intern("TUP_ARCH"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == pup::ARCH);
    }

    SECTION("returns config value when CONFIG_TUP_ARCH is set")
    {
        config_vars.set("TUP_ARCH", "arm64");

        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Config, intern("TUP_ARCH"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "arm64");
    }

    SECTION("env var takes highest priority over config")
    {
        auto env = EnvGuard { "TUP_ARCH", "env-arch" };
        config_vars.set("TUP_ARCH", "config-arch");

        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Config, intern("TUP_ARCH"), {} } });

        auto result = expand(ctx,expr);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "env-arch");
    }
}

TEST_CASE("Evaluator pattern expansion", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    // For foreach rules, all_inputs has just one element (the current input)
    auto flags = PatternFlags {
        .input_base = "foo.c",
        .input_noext = "foo",
        .input_ext = "c",
        .input_dir = "src",
        .all_inputs = { "src/foo.c" },
        .all_outputs = { "build/foo.o" },
    };

    SECTION("%f - input filename")
    {
        auto result = expand_pattern(ctx,"gcc -c %f -o %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "gcc -c src/foo.c -o build/foo.o");
    }

    SECTION("%B - basename without extension")
    {
        auto result = expand_pattern(ctx,"%B.o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "foo.o");
    }

    SECTION("%% escape")
    {
        auto result = expand_pattern(ctx,"100%%", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "100%");
    }

    SECTION("%Nf - N-th input (single)")
    {
        auto result = expand_pattern(ctx,"%1f", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "src/foo.c");
    }

    // Note: %i is for order-only inputs, not yet implemented
}

TEST_CASE("Evaluator pattern expansion - multiple inputs", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    // For non-foreach rules, all_inputs has all input files
    auto flags = PatternFlags {
        .input_base = "a.c",
        .input_noext = "a",
        .input_ext = "c",
        .input_dir = "",
        .all_inputs = { "a.c", "b.c", "c.c" },
        .all_outputs = { "out.o" },
    };

    SECTION("%f - all inputs")
    {
        auto result = expand_pattern(ctx,"gcc -c %f -o %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "gcc -c a.c b.c c.c -o out.o");
    }

    SECTION("%Nf - N-th input")
    {
        auto result = expand_pattern(ctx,"%1f %2f %3f", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "a.c b.c c.c");
    }

    // Note: %i is for order-only inputs, not yet implemented
}

TEST_CASE("Evaluator pattern expansion - all outputs", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    SECTION("%o - multiple outputs, space separated")
    {
        auto flags = PatternFlags {
            .all_outputs = { "a.o", "b.o", "c.o" },
        };

        auto result = expand_pattern(ctx, "ar rcs %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "ar rcs a.o b.o c.o");
    }

    SECTION("%o - single output")
    {
        auto flags = PatternFlags {
            .all_outputs = { "out.o" },
        };

        auto result = expand_pattern(ctx, "gcc -o %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "gcc -o out.o");
    }

    SECTION("%o - no outputs is refused")
    {
        auto flags = PatternFlags {};

        auto result = expand_pattern(ctx, "touch %o", flags);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("%o alongside %f, each covering its whole list")
    {
        auto flags = PatternFlags {
            .all_inputs = { "a.c", "b.c" },
            .all_outputs = { "a.o", "b.o" },
        };

        auto result = expand_pattern(ctx, "gen %f -o %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "gen a.c b.c -o a.o b.o");
    }
}

TEST_CASE("Evaluator pattern expansion - numbered outputs", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    SECTION("%No - N-th output (multiple)")
    {
        auto flags = PatternFlags {
            .all_outputs = { "a.o", "b.o", "c.o" },
        };

        auto result = expand_pattern(ctx,"%1o %2o %3o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "a.o b.o c.o");
    }

    SECTION("%No - single output")
    {
        auto flags = PatternFlags {
            .all_outputs = { "out.o" },
        };

        auto result = expand_pattern(ctx,"%1o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "out.o");
    }

    SECTION("%No - out of bounds produces empty")
    {
        auto flags = PatternFlags {
            .all_outputs = { "only.o" },
        };

        auto result = expand_pattern(ctx,"%2o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "");
    }

    SECTION("%No - mixed with other flags")
    {
        auto flags = PatternFlags {
            .all_inputs = { "src.c" },
            .all_outputs = { "a.o", "b.o" },
        };

        auto result = expand_pattern(ctx,"gen %f -o %1o %2o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "gen src.c -o a.o b.o");
    }
}

TEST_CASE("Evaluator pattern expansion - glob match", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    SECTION("%g - simple glob match")
    {
        auto flags = PatternFlags {
            .glob_match = "hello",
        };
        auto result = expand_pattern(ctx,"%g", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "hello");
    }

    SECTION("%g - suffix pattern match")
    {
        // Pattern: *_test.c, Input: foo_test.c, Match: foo
        auto flags = PatternFlags {
            .glob_match = "foo",
        };
        auto result = expand_pattern(ctx,"%g.o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "foo.o");
    }

    SECTION("%g - empty when not set")
    {
        auto flags = PatternFlags {
        };
        auto result = expand_pattern(ctx,"%g", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "");
    }

    SECTION("%g - combined with other flags")
    {
        auto flags = PatternFlags {
            .input_base = "foo_test.c",
            .input_noext = "foo_test",
            .glob_match = "foo",
            .all_inputs = { "foo_test.c" },
            .all_outputs = { "foo.o" },
        };
        auto result = expand_pattern(ctx,"compile %f -DNAME=%g -o %o", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "compile foo_test.c -DNAME=foo -o foo.o");
    }
}

TEST_CASE("Evaluator conditionals", "[eval]")
{
    auto vars = VarDb {};
    auto config_vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars, .config_vars = &config_vars };


    SECTION("ifdef - true when defined")
    {
        vars.set("DEBUG", "1");

        auto cond = Conditional {};
        cond.kind = Conditional::Kind::Ifdef;
        cond.var_name = intern("DEBUG");

        REQUIRE(evaluate_condition(ctx,cond));
    }

    SECTION("ifdef - false when undefined")
    {
        auto cond = Conditional {};
        cond.kind = Conditional::Kind::Ifdef;
        cond.var_name = intern("UNDEFINED");

        REQUIRE_FALSE(evaluate_condition(ctx,cond));
    }

    SECTION("ifndef - true when undefined")
    {
        auto cond = Conditional {};
        cond.kind = Conditional::Kind::Ifndef;
        cond.var_name = intern("UNDEFINED");

        REQUIRE(evaluate_condition(ctx,cond));
    }

    SECTION("ifeq - true when equal")
    {
        vars.set("CC", "gcc");

        auto cond = Conditional {};
        cond.kind = Conditional::Kind::Ifeq;
        cond.lhs.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("CC"), {} } });
        cond.rhs.parts.push_back(Expression::Literal { intern("gcc") });

        REQUIRE(evaluate_condition(ctx,cond));
    }

    SECTION("ifeq - false when not equal")
    {
        vars.set("CC", "clang");

        auto cond = Conditional {};
        cond.kind = Conditional::Kind::Ifeq;
        cond.lhs.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("CC"), {} } });
        cond.rhs.parts.push_back(Expression::Literal { intern("gcc") });

        REQUIRE_FALSE(evaluate_condition(ctx,cond));
    }

    SECTION("ifneq - true when not equal")
    {
        vars.set("CC", "clang");

        auto cond = Conditional {};
        cond.kind = Conditional::Kind::Ifneq;
        cond.lhs.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("CC"), {} } });
        cond.rhs.parts.push_back(Expression::Literal { intern("gcc") });

        REQUIRE(evaluate_condition(ctx,cond));
    }
}

TEST_CASE("Evaluator group resolution", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    SECTION("resolve_group callback for {name}")
    {
        ctx.resolve_group = [](std::string_view name) -> pup::Vec<pup::StringId> {
            if (name == "objs")
                return { intern("a.o"), intern("b.o"), intern("c.o") };
            return {};
        };

        auto pattern = PathPattern {};
        pattern.is_group = true;
        pattern.group_name = intern("objs");

        auto result = expand_path(ctx,pattern);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 3);
        REQUIRE(sv((*result)[0]) == "a.o");
        REQUIRE(sv((*result)[1]) == "b.o");
        REQUIRE(sv((*result)[2]) == "c.o");
    }

    SECTION("resolve_order_only_group callback for <name>")
    {
        ctx.resolve_order_only_group = [](std::string_view name) -> pup::Vec<pup::StringId> {
            if (name == "gen-headers")
                return { intern("generated/config.h"), intern("generated/version.h") };
            return {};
        };

        auto pattern = PathPattern {};
        pattern.is_order_only_group = true;
        pattern.group_name = intern("gen-headers");

        auto result = expand_path(ctx,pattern);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE(sv((*result)[0]) == "generated/config.h");
        REQUIRE(sv((*result)[1]) == "generated/version.h");
    }

    SECTION("unresolved group returns empty")
    {
        auto pattern = PathPattern {};
        pattern.is_order_only_group = true;
        pattern.group_name = intern("nonexistent");

        auto result = expand_path(ctx,pattern);
        REQUIRE(result.has_value());
        REQUIRE(result->empty());
    }
}

TEST_CASE("Evaluator %<group> pattern expansion", "[eval]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    ctx.resolve_order_only_group = [](std::string_view name) -> pup::Vec<pup::StringId> {
        if (name == "headers")
            return { intern("inc/a.h"), intern("inc/b.h") };
        return {};
    };

    auto flags = PatternFlags {
        .all_inputs = { "foo.c" },
        .all_outputs = { "foo.o" },
    };

    SECTION("%<group> expands to group paths")
    {
        auto result = expand_pattern(ctx,"echo %<headers>", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "echo inc/a.h inc/b.h");
    }

    SECTION("%<nonexistent> expands to empty")
    {
        auto result = expand_pattern(ctx,"echo %<nonexistent>", flags);
        REQUIRE(result.has_value());
        REQUIRE(sv(*result) == "echo ");
    }
}

// =============================================================================
// TUP_VARIANT_OUTPUTDIR tests
// Based on tup test t8108-variant-outputdir.sh
// =============================================================================

TEST_CASE("TUP_VARIANT_OUTPUTDIR expansion - no variant", "[eval][variant]")
{
    // Without variant, TUP_VARIANT_OUTPUTDIR should be "."
    // This matches tup behavior where outputs go to current directory

    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };
    ctx.tup_variant_outputdir = intern(".");



    auto result = expand(ctx,"$(TUP_VARIANT_OUTPUTDIR)");
    REQUIRE(result.has_value());
    CHECK(sv(*result) == ".");
}

TEST_CASE("TUP_VARIANT_OUTPUTDIR expansion - in-tree variant", "[eval][variant]")
{
    // For in-tree variant build (e.g., build/ directory):
    // From sub/dir Tupfile, TUP_VARIANT_OUTPUTDIR = ../../build/sub/dir
    // This is the relative path from source sub/dir to output build/sub/dir

    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };
    ctx.tup_variant_outputdir = intern("../../build/sub/dir");



    auto result = expand(ctx,"$(TUP_VARIANT_OUTPUTDIR)");
    REQUIRE(result.has_value());
    CHECK(sv(*result) == "../../build/sub/dir");
}

TEST_CASE("TUP_VARIANT_OUTPUTDIR in command expansion", "[eval][variant]")
{
    // Test that TUP_VARIANT_OUTPUTDIR expands correctly in commands
    // This simulates: echo -o $(TUP_VARIANT_OUTPUTDIR)/out.txt

    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };
    ctx.tup_variant_outputdir = intern("../../build/sub/dir");



    auto result = expand(ctx,"echo -o $(TUP_VARIANT_OUTPUTDIR)/out.txt");
    REQUIRE(result.has_value());
    CHECK(sv(*result) == "echo -o ../../build/sub/dir/out.txt");
}

TEST_CASE("TUP_VARIANTDIR vs TUP_VARIANT_OUTPUTDIR", "[eval][variant]")
{
    // TUP_VARIANTDIR: relative path to variant's version of *included file's* directory
    // TUP_VARIANT_OUTPUTDIR: relative path to variant's version of *current Tupfile's* directory
    //
    // From tup test t8108:
    // - TUP_CWD = ../../rules (relative to included file)
    // - TUP_VARIANTDIR = ../../build/rules (variant's rules directory)
    // - TUP_VARIANT_OUTPUTDIR = ../../build/sub/dir (variant's output for this Tupfile)

    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };
    ctx.tup_cwd = intern("../../rules");
    ctx.tup_variantdir = intern("../../build/rules");
    ctx.tup_variant_outputdir = intern("../../build/sub/dir");



    SECTION("TUP_CWD expands to included file's relative path")
    {
        auto result = expand(ctx,"$(TUP_CWD)");
        REQUIRE(result.has_value());
        CHECK(sv(*result) == "../../rules");
    }

    SECTION("TUP_VARIANTDIR expands to variant's included file directory")
    {
        auto result = expand(ctx,"$(TUP_VARIANTDIR)");
        REQUIRE(result.has_value());
        CHECK(sv(*result) == "../../build/rules");
    }

    SECTION("TUP_VARIANT_OUTPUTDIR expands to variant's current directory")
    {
        auto result = expand(ctx,"$(TUP_VARIANT_OUTPUTDIR)");
        REQUIRE(result.has_value());
        CHECK(sv(*result) == "../../build/sub/dir");
    }

    SECTION("All three in same command")
    {
        auto result = expand(ctx,"CWD=$(TUP_CWD) VARIANTDIR=$(TUP_VARIANTDIR) OUTPUTDIR=$(TUP_VARIANT_OUTPUTDIR)");
        REQUIRE(result.has_value());
        CHECK(sv(*result) == "CWD=../../rules VARIANTDIR=../../build/rules OUTPUTDIR=../../build/sub/dir");
    }
}

TEST_CASE("Evaluator undefined variable handling", "[eval][error]")
{
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };


    SECTION("undefined variable expands to empty string")
    {
        auto result = expand(ctx,"prefix_$(UNDEFINED)_suffix");
        REQUIRE(result.has_value());
        CHECK(sv(*result) == "prefix__suffix");
    }

    SECTION("multiple undefined variables")
    {
        auto result = expand(ctx,"$(A)$(B)$(C)");
        REQUIRE(result.has_value());
        CHECK(sv(*result) == "");
    }

    SECTION("mixed defined and undefined")
    {
        vars.set("DEFINED", "value");
        auto result = expand(ctx,"$(DEFINED)-$(UNDEFINED)");
        REQUIRE(result.has_value());
        CHECK(sv(*result) == "value-");
    }
}
