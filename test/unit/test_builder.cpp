// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/builder.hpp"
#include "pup/parser/eval.hpp"
#include "pup/parser/parser.hpp"

#include <filesystem>
#include <fstream>
#include <random>

using namespace pup;
using namespace pup::graph;
using namespace pup::parser;

namespace fs = std::filesystem;

namespace {
auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }
} // namespace

namespace {

/// Helper to create a simple PathPattern for a literal path
auto make_path_pattern(std::string_view path) -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.path.parts.push_back(Expression::Literal { intern(path) });
    return pattern;
}

/// Helper to create a {group} bin reference
auto make_group_pattern(std::string_view name) -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.is_group = true;
    pattern.group_name = intern(name);
    return pattern;
}

/// Helper to create an <order-only-group> reference with optional path prefix
auto make_order_only_group_pattern(std::string_view name, std::string_view path = "") -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.is_order_only_group = true;
    pattern.group_name = intern(name);
    if (!path.empty()) {
        pattern.path.parts.push_back(Expression::Literal { intern(path) });
    }
    return pattern;
}

/// Helper to create an order-only group pattern with variable prefix
auto make_order_only_group_var_pattern(std::string_view name, std::string_view var_name, std::string_view suffix = "") -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.is_order_only_group = true;
    pattern.group_name = intern(name);
    pattern.path.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern(var_name), {} } });
    if (!suffix.empty()) {
        pattern.path.parts.push_back(Expression::Literal { intern(suffix) });
    }
    return pattern;
}

/// Helper to create a Statement containing a Rule
auto make_rule_statement(Rule rule) -> std::unique_ptr<Statement>
{
    auto stmt = std::make_unique<Statement>();
    stmt->content = std::move(rule);
    return stmt;
}

/// Test fixture with temporary directory
class BuilderTestFixture {
public:
    BuilderTestFixture()
    {
        test_root_ = fs::temp_directory_path()
            / ("pup_test_builder_" + std::to_string(std::random_device {}()));
        fs::create_directories(test_root_);
        fs::create_directories(test_root_ / "src");
        fs::create_directories(test_root_ / "include");
        fs::create_directories(test_root_ / "include" / "generated");
        fs::create_directories(test_root_ / "modules" / "kernel");
        fs::create_directories(test_root_ / "modules" / "app");
        fs::create_directories(test_root_ / "build-variant");
        fs::create_directories(test_root_ / "build-variant" / "modules" / "kernel");
    }

    ~BuilderTestFixture()
    {
        std::error_code ec;
        fs::remove_all(test_root_, ec);
    }

    auto root() const -> fs::path const& { return test_root_; }
    auto root_str() const -> std::string { return test_root_.generic_string(); }

    auto create_file(fs::path const& rel_path) -> void
    {
        auto full = test_root_ / rel_path;
        fs::create_directories(full.parent_path());
        auto file = std::ofstream { full };
        file << "test";
    }

    /// Create a Tupfile path for the given directory (relative to test root)
    auto tupfile_path(std::string_view dir) const -> std::string
    {
        if (dir.empty() || dir == ".") {
            return (test_root_ / "Tupfile").generic_string();
        }
        return (test_root_ / dir / "Tupfile").generic_string();
    }

private:
    fs::path test_root_;
};

} // namespace

// =============================================================================
// normalize_group_dir behavior tests (verify the function's edge cases)
// These test the exact behavior that the refactoring regression broke
// =============================================================================

TEST_CASE("GraphBuilder order-only group - case 1: empty pattern.path", "[e2e][builder][group][critical]")
{
    // Case 1: is_order_only_group with empty pattern.path
    // Should use current_dir directly, NOT normalize_group_dir("")
    // This is the case that was broken by the refactoring

    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // First Tupfile: modules/kernel defines <local-group>
    auto tupfile1 = Tupfile {};
    tupfile1.filename = intern(fixture.tupfile_path("modules/kernel"));
    fixture.create_file("modules/kernel/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("gen-header") });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("generated.h") });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = intern("local-group");
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = add_tupfile(bs, tupfile1, ctx, builder_state);
    (void)r1;

    // Second Tupfile: also in modules/kernel, references <local-group> (empty path)
    // GroupKey should be {"modules/kernel", "local-group"}
    auto tupfile2 = Tupfile {};
    tupfile2.filename = intern(fixture.tupfile_path("modules/kernel"));

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("compile") });

    // Empty path - this is the critical case
    auto input = make_order_only_group_pattern("local-group");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("output.o") });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = add_tupfile(bs, tupfile2, ctx, builder_state);
    REQUIRE(r2.has_value());
}

TEST_CASE("GraphBuilder order-only group - case 2: non-empty pattern.path with variable", "[e2e][builder][group][critical]")
{
    // Case 2: is_order_only_group with non-empty pattern.path
    // Should use normalize_group_dir(expanded_path, current_dir, source_root)

    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    vars.set("ROOT", fixture.root().generic_string());
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // First Tupfile: include/generated defines <gen-headers>
    auto tupfile1 = Tupfile {};
    tupfile1.filename = intern(fixture.tupfile_path("include/generated"));
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("gen-config") });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("config.h") });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = intern("gen-headers");
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = add_tupfile(bs, tupfile1, ctx, builder_state);
    (void)r1;

    // Second Tupfile: modules/kernel references $(ROOT)/include/generated/<gen-headers>
    // This should normalize to {"include/generated", "gen-headers"}
    auto tupfile2 = Tupfile {};
    tupfile2.filename = intern(fixture.tupfile_path("modules/kernel"));
    fixture.create_file("modules/kernel/Tupfile");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("compile") });

    // Non-empty path with variable expansion
    auto input = make_order_only_group_var_pattern("gen-headers", "ROOT", "/include/generated/");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("output.o") });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = add_tupfile(bs, tupfile2, ctx, builder_state);
    REQUIRE(r2.has_value());
}

TEST_CASE("GraphBuilder order-only group - case 3: path/<group> pattern", "[e2e][builder][group][critical]")
{
    // Case 3: After expand_path, path contains literal <groupname> suffix
    // Should ALWAYS use normalize_group_dir(dir_part, current_dir, source_root)
    // Even when dir_part is empty

    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // First: define group in include/generated
    auto tupfile1 = Tupfile {};
    tupfile1.filename = intern(fixture.tupfile_path("include/generated"));
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("gen-version") });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("version.h") });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = intern("gen-headers");
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = add_tupfile(bs, tupfile1, ctx, builder_state);
    (void)r1;

    // Second: reference with relative path ../../include/generated/<gen-headers>
    // from modules/kernel
    auto tupfile2 = Tupfile {};
    tupfile2.filename = intern(fixture.tupfile_path("modules/kernel"));
    fixture.create_file("modules/kernel/Tupfile");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("compile") });

    // This is a literal path pattern - the <gen-headers> is NOT parsed as is_order_only_group
    // It gets expanded and then matched by the rfind('<') logic
    auto input = make_path_pattern("../../include/generated/<gen-headers>");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("output.o") });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = add_tupfile(bs, tupfile2, ctx, builder_state);
    REQUIRE(r2.has_value());
}

// =============================================================================
// Bin {group} tests
// =============================================================================

TEST_CASE("GraphBuilder bin group reference {name}", "[e2e][builder][group]")
{
    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // Create source files
    fixture.create_file("src/a.c");
    fixture.create_file("src/b.c");

    // Tupfile that compiles *.c -> {objs}, then links {objs} -> app
    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    // Rule 1: compile a.c -> a.o into {objs}
    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("cc -c a.c -o a.o") });
    rule1.inputs.push_back(make_path_pattern("a.c"));
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("a.o") });
    rule1.outputs.push_back(out1);
    rule1.output_group = intern("objs");
    tupfile.statements.push_back(make_rule_statement(std::move(rule1)));

    // Rule 2: compile b.c -> b.o into {objs}
    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("cc -c b.c -o b.o") });
    rule2.inputs.push_back(make_path_pattern("b.c"));
    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("b.o") });
    rule2.outputs.push_back(out2);
    rule2.output_group = intern("objs");
    tupfile.statements.push_back(make_rule_statement(std::move(rule2)));

    // Rule 3: link {objs} -> app
    auto rule3 = Rule {};
    rule3.command.parts.push_back(Expression::Literal { intern("cc -o app") });
    rule3.inputs.push_back(make_group_pattern("objs"));
    auto out3 = PathPattern {};
    out3.path.parts.push_back(Expression::Literal { intern("app") });
    rule3.outputs.push_back(out3);
    tupfile.statements.push_back(make_rule_statement(std::move(rule3)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Verify the link command has both .o files as inputs
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    auto link_cmd_count = 0;
    for (auto id : commands) {
        auto cmd_str = expand_instruction(bs.graph, id);
        if (sv(cmd_str).find("-o app") != std::string_view::npos) {
            auto inputs = get_inputs(bs.graph, id);
            auto o_count = 0;
            for (auto input_id : inputs) {
                if (sv(get_full_path(bs.graph, input_id)).find(".o") != std::string::npos) {
                    ++o_count;
                }
            }
            REQUIRE(o_count == 2);
            ++link_cmd_count;
        }
    }
    REQUIRE(link_cmd_count == 1);
}

// =============================================================================
// Glob expansion tests
// =============================================================================

TEST_CASE("GraphBuilder glob expansion - filesystem", "[e2e][builder][glob]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/foo.c");
    fixture.create_file("src/bar.c");
    fixture.create_file("src/baz.h"); // Should not match *.c

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.foreach_ = true;
    rule.command.parts.push_back(Expression::Literal { intern("cc -c") });
    rule.inputs.push_back(make_path_pattern("*.c"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("%B.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Should have 2 compile commands (foo.c and bar.c)
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    REQUIRE(commands.size() == 2);
}

TEST_CASE("GraphBuilder glob expansion - generated files", "[e2e][builder][glob]")
{
    auto fixture = BuilderTestFixture {};

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern((fixture.root() / "build-variant").string()),
        .config_path = pup::StringId::Empty,
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // First Tupfile: generate some .h files
    auto tupfile1 = Tupfile {};
    tupfile1.filename = intern(fixture.tupfile_path("include/generated"));
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("gen-config") });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("config.h") });
    rule1.outputs.push_back(out1);
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("gen-version") });
    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("version.h") });
    rule2.outputs.push_back(out2);
    tupfile1.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r1 = add_tupfile(bs, tupfile1, ctx, builder_state);
    REQUIRE(r1.has_value());

    // Verify generated files exist in graph
    auto generated = nodes_of_type(bs.graph,NodeType::Generated);
    REQUIRE(generated.size() == 2);
}

// =============================================================================
// tup.config special case
// =============================================================================

TEST_CASE("GraphBuilder tup.config in variant directory", "[e2e][builder][config]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("build-variant/tup.config");
    fixture.create_file("src/main.c");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern((fixture.root() / "build-variant").string()),
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { intern("cat tup.config") });
    rule.inputs.push_back(make_path_pattern("../tup.config"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("config.txt") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    // Should find tup.config in the variant directory
    REQUIRE(result.has_value());
}

// =============================================================================
// Exclusion pattern tests
// =============================================================================

TEST_CASE("GraphBuilder exclusion patterns - explicit file", "[e2e][builder][exclusion]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/foo.c");
    fixture.create_file("src/bar.c");
    fixture.create_file("src/baz.c"); // Will be excluded

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.foreach_ = true;
    rule.command.parts.push_back(Expression::Literal { intern("cc -c") });
    rule.inputs.push_back(make_path_pattern("*.c"));

    // Exclusion pattern - explicit filename
    auto excl = make_path_pattern("baz.c");
    excl.is_exclusion = true;
    rule.inputs.push_back(excl);

    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("%B.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Count .o outputs - baz.c should be excluded
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    auto compile_count = 0;
    auto has_baz = false;
    for (auto id : commands) {
        for (auto out_id : get_outputs(bs.graph, id)) {
            auto path = sv(get_full_path(bs.graph, out_id));
            if (path.find(".o") != std::string::npos) {
                ++compile_count;
                if (path.find("baz.o") != std::string::npos) {
                    has_baz = true;
                }
            }
        }
    }
    CHECK(compile_count == 2);
    CHECK_FALSE(has_baz);
}

TEST_CASE("GraphBuilder exclusion patterns - glob pattern", "[e2e][builder][exclusion]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/main.c");
    fixture.create_file("src/util.c");
    fixture.create_file("src/test_main.c"); // Will be excluded
    fixture.create_file("src/test_util.c"); // Will be excluded

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.foreach_ = true;
    rule.command.parts.push_back(Expression::Literal { intern("cc -c") });
    rule.inputs.push_back(make_path_pattern("*.c"));

    // Exclusion pattern - glob to exclude test files
    auto excl = make_path_pattern("test_*.c");
    excl.is_exclusion = true;
    rule.inputs.push_back(excl);

    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("%B.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Count .o outputs - test_*.c should be excluded
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    auto compile_count = 0;
    auto has_test = false;
    for (auto id : commands) {
        auto outputs = get_outputs(bs.graph,id);
        for (auto out_id : outputs) {
            auto path = sv(get_full_path(bs.graph, out_id));
            if (path.find(".o") != std::string::npos) {
                ++compile_count;
                if (path.find("test_") != std::string::npos) {
                    has_test = true;
                }
            }
        }
    }
    // Should have 2 compiles (main.c, util.c), test_*.c excluded
    CHECK(compile_count == 2);
    CHECK_FALSE(has_test);
}

TEST_CASE("GraphBuilder caret exclusion patterns for foreach", "[e2e][builder][exclusion][foreach]")
{
    // Tests that ^ prefix works as exclusion in input patterns (tup uses ^ for foreach exclusions)
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/main.c");
    fixture.create_file("src/util.c");
    fixture.create_file("src/helper_impl.c"); // Will be excluded - gets #included

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.foreach_ = true;
    rule.command.parts.push_back(Expression::Literal { intern("cc -c %f -o %o") });

    // Input pattern: *.c
    auto input = make_path_pattern("*.c");
    rule.inputs.push_back(input);

    // Caret exclusion pattern - exclude helper_impl.c (like tup's foreach ^pattern)
    auto excl = make_path_pattern("helper_impl.c");
    excl.is_output_exclusion = true; // ^ prefix sets is_output_exclusion
    rule.inputs.push_back(excl);

    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("%B.o") });
    rule.outputs.push_back(output);

    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Count .o outputs - helper_impl.c should be excluded
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    auto compile_count = 0;
    auto has_helper = false;
    for (auto id : commands) {
        auto outputs = get_outputs(bs.graph,id);
        for (auto out_id : outputs) {
            auto path = sv(get_full_path(bs.graph, out_id));
            if (path.find(".o") != std::string::npos) {
                ++compile_count;
                if (path.find("helper_impl") != std::string::npos) {
                    has_helper = true;
                }
            }
        }
    }
    // Should have 2 compiles (main.c, util.c), helper_impl.c excluded via ^
    CHECK(compile_count == 2);
    CHECK_FALSE(has_helper);
}

// =============================================================================
// Cross-directory group reference tests
// =============================================================================

TEST_CASE("GraphBuilder cross-directory order-only group with relative path", "[e2e][builder][group][cross-dir]")
{
    auto fixture = BuilderTestFixture {};

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // Tupfile in include/generated: defines <gen-headers>
    auto tupfile1 = Tupfile {};
    tupfile1.filename = intern(fixture.tupfile_path("include/generated"));
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("gen-config") });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("config.h") });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = intern("gen-headers");
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = add_tupfile(bs, tupfile1, ctx, builder_state);
    REQUIRE(r1.has_value());

    // Tupfile in modules/kernel: references ../../include/generated/<gen-headers>
    auto tupfile2 = Tupfile {};
    tupfile2.filename = intern(fixture.tupfile_path("modules/kernel"));
    fixture.create_file("modules/kernel/Tupfile");
    fixture.create_file("modules/kernel/kernel.c");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("compile kernel.c") });
    rule2.inputs.push_back(make_path_pattern("kernel.c"));

    // Order-only reference with relative path
    // Note: This is a literal path, will be matched by rfind('<') logic
    auto order_input = make_path_pattern("../../include/generated/<gen-headers>");
    rule2.order_only_inputs.push_back(order_input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("kernel.o") });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = add_tupfile(bs, tupfile2, ctx, builder_state);
    REQUIRE(r2.has_value());

    // Resolve deferred order-only edges (required after all tupfiles are parsed)
    auto resolve_result = finalize_graph(bs, builder_state);
    REQUIRE(resolve_result.has_value());

    // Verify the kernel.o command has an order-only dependency on config.h
    auto found = false;
    for (auto id : nodes_of_type(bs.graph,NodeType::Command)) {
        auto cmd_str = expand_instruction(bs.graph, id);
        if (sv(cmd_str).find("compile kernel.c") != std::string_view::npos) {
            auto order_only = get_order_only(bs.graph,id);
            // Should have config.h as order-only
            found = !order_only.empty();
            break;
        }
    }
    // This test verifies the group reference is resolved correctly
    CHECK(found);
}

// =============================================================================
// normalize_group_dir edge cases
// =============================================================================

TEST_CASE("GraphBuilder normalize_group_dir empty string returns dot", "[e2e][builder][normalize]")
{
    // This test documents the critical behavior:
    // normalize_group_dir("", current_dir, source_root) should return "."
    // NOT current_dir

    // The refactoring regression happened because the helper function
    // returned current_dir when dir_prefix was empty, but the original
    // code called normalize_group_dir("") which returns "."

    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // Case 1: Group defined at root level (directory ".")
    auto tupfile1 = Tupfile {};
    tupfile1.filename = intern(fixture.tupfile_path(""));
    fixture.create_file("Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("gen-root") });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("root.h") });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = intern("root-group");
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = add_tupfile(bs, tupfile1, ctx, builder_state);
    REQUIRE(r1.has_value());

    // Case 2: Reference with empty path from modules/kernel
    // The expanded path will be "<root-group>" (no dir prefix)
    // This should look up {".", "root-group"} NOT {"modules/kernel", "root-group"}
    auto tupfile2 = Tupfile {};
    tupfile2.filename = intern(fixture.tupfile_path("modules/kernel"));
    fixture.create_file("modules/kernel/Tupfile");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("compile") });

    // Use path pattern that after expansion is just "<root-group>"
    auto input = make_path_pattern("<root-group>");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("output.o") });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = add_tupfile(bs, tupfile2, ctx, builder_state);
    REQUIRE(r2.has_value());

    // Resolve deferred order-only edges (required after all tupfiles are parsed)
    auto resolve_result = finalize_graph(bs, builder_state);
    REQUIRE(resolve_result.has_value());

    // The critical verification: check that the group WAS found
    // If normalize_group_dir("") returns "." then it will match
    // If it returns current_dir ("modules/kernel") then it won't match
    auto found_order_only = false;
    for (auto id : nodes_of_type(bs.graph,NodeType::Command)) {
        auto cmd_str = expand_instruction(bs.graph, id);
        if (sv(cmd_str).find("compile") != std::string_view::npos) {
            auto order_only = get_order_only(bs.graph,id);
            if (!order_only.empty()) {
                found_order_only = true;
            }
            break;
        }
    }
    CHECK(found_order_only);
}

// =============================================================================
// Variant build path resolution
// =============================================================================

TEST_CASE("GraphBuilder variant output mapping", "[e2e][builder][variant]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/main.c");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern((fixture.root() / "build-variant").string()),
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { intern("cc -c main.c -o main.o") });
    rule.inputs.push_back(make_path_pattern("main.c"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("main.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Verify output is in variant directory
    auto generated = nodes_of_type(bs.graph,NodeType::Generated);
    REQUIRE(generated.size() == 1);

    // Output should be under build-variant/src/main.o or absolute path
    CHECK(sv(get_full_path(bs.graph, generated[0])).find("main.o") != std::string::npos);
}

// =============================================================================
// Deep current_dir tests (regression prevention)
// =============================================================================

TEST_CASE("GraphBuilder deep directory with parent references", "[e2e][builder][deep-dir]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("include/common.h");
    fs::create_directories(fixture.root() / "modules" / "app" / "sub" / "deep");
    fixture.create_file("modules/app/sub/deep/impl.c");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // Tupfile in deeply nested directory
    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("modules/app/sub/deep"));
    fixture.create_file("modules/app/sub/deep/Tupfile");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { intern("cc -c impl.c") });
    rule.inputs.push_back(make_path_pattern("impl.c"));
    // Reference header with multiple parent refs
    rule.inputs.push_back(make_path_pattern("../../../../include/common.h"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("impl.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Verify the header was resolved correctly
    auto files = nodes_of_type(bs.graph,NodeType::File);
    auto found_header = false;
    for (auto id : files) {
        auto path = sv(get_full_path(bs.graph, id));
        if (path.find("common.h") != std::string::npos) {
            CHECK(path == "include/common.h");
            found_header = true;
            break;
        }
    }
    REQUIRE(found_header);
}

TEST_CASE("GraphBuilder directory node creation", "[e2e][builder][dir-nodes]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/util/helpers.c");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { intern("cc -c util/helpers.c") });
    rule.inputs.push_back(make_path_pattern("util/helpers.c"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("helpers.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Find the helpers.c file node
    auto files = nodes_of_type(bs.graph,NodeType::File);
    NodeId helpers_id = 0;
    for (auto id : files) {
        if (sv(get_full_path(bs.graph, id)).find("helpers.c") != std::string::npos) {
            helpers_id = id;
            break;
        }
    }
    REQUIRE(helpers_id != 0);

    // Verify the node has name and parent_dir set
    CHECK(sv(get<Name>(bs.graph, helpers_id)) == "helpers.c");
    auto helpers_parent_dir_id = get_parent_dir(bs.graph, helpers_id);
    CHECK(helpers_parent_dir_id != 0); // Not root

    // Verify the parent directory node exists
    CHECK(get<NodeType>(bs.graph, helpers_parent_dir_id) == NodeType::Directory);
    CHECK(sv(get<Name>(bs.graph, helpers_parent_dir_id)) == "util");

    // Verify we can find the file via (parent_dir, name)
    auto found = find_by_dir_name(bs.graph, helpers_parent_dir_id, "helpers.c");
    REQUIRE(found.has_value());
    CHECK(*found == helpers_id);
}

// =============================================================================
// Out-of-tree build tests (-B flag)
// =============================================================================

TEST_CASE("GraphBuilder out-of-tree build outputs use relative paths", "[e2e][builder][variant]")
{
    // When using -B for out-of-tree builds, output paths in the graph are stored
    // source-root-relative (e.g., "src/main.o"). Variant mapping is applied at I/O time.
    // This ensures inputs and outputs resolve to the same node (in-place Ghost→Generated upgrade).

    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    // Simulate -B build-variant
    auto output_root = (fixture.root() / "build-variant").string();
    fs::create_directories(fs::path { std::string(output_root) } / "src");

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern(output_root),
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));
    fixture.create_file("src/Tupfile");
    fixture.create_file("src/main.c");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { intern("cc -c main.c -o main.o") });
    rule.inputs.push_back(make_path_pattern("main.c"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("main.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Find the output node
    auto generated = nodes_of_type(bs.graph,NodeType::Generated);
    REQUIRE(generated.size() >= 1);

    NodeId output_id = 0;
    for (auto id : generated) {
        if (sv(get<Name>(bs.graph, id)) == "main.o") {
            output_id = id;
            break;
        }
    }
    REQUIRE(output_id != 0);

    // The full path is source-root-relative: "src/main.o"
    // Variant mapping (to "build-variant/src/main.o") is applied at command expansion time
    auto full_path = get_full_path(bs.graph, output_id);
    CHECK(sv(full_path) == "src/main.o");

    // It should NOT be an absolute path
    CHECK(sv(full_path)[0] != '/');
}

TEST_CASE("GraphBuilder out-of-tree cross-directory generated file reference", "[e2e][builder][variant][critical]")
{
    // Test case: Tupfile in output/hex references generated file from boot/
    // With source-root-relative storage, output/hex/Tupfile references
    // ../../boot/boot.hex which resolves to the same node as boot/Tupfile's output

    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    // Create directory structure matching spos
    fs::create_directories(fixture.root() / "boot");
    fs::create_directories(fixture.root() / "output" / "hex");
    fs::create_directories(fixture.root() / "build-variant" / "boot");
    fs::create_directories(fixture.root() / "build-variant" / "output" / "hex");

    auto output_root = (fixture.root() / "build-variant").string();
    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern(output_root),
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // First Tupfile: boot/Tupfile generates boot.hex
    auto tupfile1 = Tupfile {};
    tupfile1.filename = intern((fixture.root() / "boot" / "Tupfile").string());
    fixture.create_file("boot/Tupfile");
    fixture.create_file("boot/boot.elf");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { intern("objcopy -O ihex boot.elf boot.hex") });
    rule1.inputs.push_back(make_path_pattern("boot.elf"));
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { intern("boot.hex") });
    rule1.outputs.push_back(out1);
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = add_tupfile(bs, tupfile1, ctx, builder_state);
    REQUIRE(r1.has_value());

    // Find the generated boot.hex node
    auto generated = nodes_of_type(bs.graph,NodeType::Generated);
    NodeId boot_hex_id = 0;
    for (auto id : generated) {
        if (sv(get<Name>(bs.graph, id)) == "boot.hex") {
            boot_hex_id = id;
            break;
        }
    }
    REQUIRE(boot_hex_id != 0);

    // The output path is source-root-relative: "boot/boot.hex"
    auto boot_hex_path = get_full_path(bs.graph, boot_hex_id);
    CHECK(sv(boot_hex_path) == "boot/boot.hex");

    // Second Tupfile: output/hex/Tupfile references ../../boot/boot.hex (source-relative)
    auto tupfile2 = Tupfile {};
    tupfile2.filename = intern((fixture.root() / "output" / "hex" / "Tupfile").string());
    fixture.create_file("output/hex/Tupfile");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { intern("srec_cat boot.hex -o combined.hex") });
    // Reference the generated file from boot/ using source-relative path
    rule2.inputs.push_back(make_path_pattern("../../boot/boot.hex"));
    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { intern("combined.hex") });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = add_tupfile(bs, tupfile2, ctx, builder_state);
    REQUIRE(r2.has_value());

    // Find the command node
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    NodeId srec_cmd_id = 0;
    for (auto id : commands) {
        auto cmd_str = expand_instruction(bs.graph, id);
        if (sv(cmd_str).find("srec_cat") != std::string_view::npos) {
            srec_cmd_id = id;
            break;
        }
    }
    REQUIRE(srec_cmd_id != 0);

    // The srec_cat command should have boot.hex as an input
    // This verifies that the input path resolved to the same node as the output
    auto inputs = get_inputs(bs.graph, srec_cmd_id);
    bool found_boot_hex = false;
    for (auto input_id : inputs) {
        if (input_id == boot_hex_id) {
            found_boot_hex = true;
            break;
        }
    }
    CHECK(found_boot_hex);
}

TEST_CASE("GraphBuilder TUP_VARIANT_OUTPUTDIR matches tup behavior", "[e2e][builder][variant][critical]")
{
    // Based on tup test t8108-variant-outputdir.sh
    // Outputs are stored at source-relative paths (same as inputs).
    // Variant mapping happens at command expansion time via transform_output_path().

    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    // Create directory structure: sub/dir with build variant
    fs::create_directories(fixture.root() / "sub" / "dir");
    fs::create_directories(fixture.root() / "build" / "sub" / "dir");

    // In-tree variant build
    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern((fixture.root() / "build").string()),
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // Set build root name for variant build (normally done by builder.build())
    set_build_root_name(bs, "build");

    // Set TUP_VARIANT_OUTPUTDIR as tup would for sub/dir Tupfile with build variant
    // From sub/dir, path to build/sub/dir is ../../build/sub/dir
    ctx.tup_variant_outputdir = intern("../../build/sub/dir");

    auto tupfile = Tupfile {};
    tupfile.filename = intern((fixture.root() / "sub" / "dir" / "Tupfile").string());
    fixture.create_file("sub/dir/Tupfile");

    // Rule that outputs to current directory (which should be build/sub/dir)
    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { intern("echo test > out.txt") });
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("out.txt") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Find the output node
    auto generated = nodes_of_type(bs.graph,NodeType::Generated);
    NodeId output_id = 0;
    for (auto id : generated) {
        if (sv(get<Name>(bs.graph, id)) == "out.txt") {
            output_id = id;
            break;
        }
    }
    REQUIRE(output_id != 0);

    // Output is stored at variant-mapped path (via walk_to_file_node with variant_output_dir)
    auto full_path = get_full_path(bs.graph, output_id);
    CHECK(sv(full_path) == "build/sub/dir/out.txt");
}

// =============================================================================
// Path simplification tests
// =============================================================================

TEST_CASE("GraphBuilder path simplification at root", "[e2e][builder][paths]")
{
    // Test that paths at project root stay as-is (no transformation needed)

    auto fixture = BuilderTestFixture {};
    fixture.create_file("main.c");
    fixture.create_file("Tupfile");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path(""));

    // Rule: : main.c |> gcc -c %f -o %o |> main.o
    auto rule = Rule {};
    rule.inputs.push_back(make_path_pattern("main.c"));
    rule.command.parts.push_back(Expression::Literal { intern("gcc -c %f -o %o") });
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("main.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    REQUIRE(!commands.empty());

    // At root, paths should be direct (no ../ prefixes)
    auto cmd_str = expand_instruction(bs.graph, commands[0]);
    CHECK(sv(cmd_str).find("../") == std::string_view::npos);
    CHECK(sv(cmd_str).find("-c main.c") != std::string_view::npos);
    CHECK(sv(cmd_str).find("-o main.o") != std::string_view::npos);
}

TEST_CASE("GraphBuilder path simplification in subdirectory commands", "[e2e][builder][paths]")
{
    // Test that local files in subdirectory builds use simplified paths
    // e.g., "add.c" not "../../src/lib/add.c" when Tupfile is in src/lib/

    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/lib/add.c");
    fixture.create_file("src/lib/Tupfile");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src/lib"));

    // Rule: : add.c |> gcc -c %f -o %o |> add.o
    auto rule = Rule {};
    rule.inputs.push_back(make_path_pattern("add.c"));
    rule.command.parts.push_back(Expression::Literal { intern("gcc -c %f -o %o") });
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("add.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Find the command node
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    REQUIRE(!commands.empty());

    // Command should use "add.c" not "../../src/lib/add.c"
    auto cmd_str = expand_instruction(bs.graph, commands[0]);
    CHECK(sv(cmd_str).find("../../src/lib/add.c") == std::string_view::npos);
    CHECK(sv(cmd_str).find("-c add.c") != std::string_view::npos);
    CHECK(sv(cmd_str).find("-o add.o") != std::string_view::npos);
}

TEST_CASE("GraphBuilder path simplification - cross-directory reference", "[e2e][builder][paths]")
{
    // Test that cross-directory references resolve correctly
    // When Tupfile is in src/lib/ and references ../util/helper.c
    // The path becomes root-relative (../../src/util/helper.c) which is
    // functionally correct from the working directory

    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/lib/main.c");
    fixture.create_file("src/lib/Tupfile");
    fixture.create_file("src/util/helper.c");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src/lib"));

    // Rule: : main.c ../util/helper.c |> gcc -c %f -o %o |> app.o
    auto rule = Rule {};
    rule.inputs.push_back(make_path_pattern("main.c"));
    rule.inputs.push_back(make_path_pattern("../util/helper.c"));
    rule.command.parts.push_back(Expression::Literal { intern("gcc -c %f -o %o") });
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("app.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Find the command node
    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    REQUIRE(!commands.empty());

    // Local file should be simplified, cross-directory uses root-relative path
    auto cmd_str = expand_instruction(bs.graph, commands[0]);
    INFO("Command: " << std::string { sv(cmd_str) });
    CHECK(sv(cmd_str).find("main.c") != std::string_view::npos);
    // Cross-directory reference becomes root-relative: ../../src/util/helper.c
    CHECK(sv(cmd_str).find("src/util/helper.c") != std::string_view::npos);
}

TEST_CASE("GraphBuilder path simplification in variant build", "[e2e][builder][paths][variant]")
{
    // Test that in variant builds:
    // - Input paths (source files) are still simplified
    // - Output paths go to variant directory

    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/lib/add.c");
    fixture.create_file("src/lib/Tupfile");
    fs::create_directories(fixture.root() / "build" / "src" / "lib");

    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    // Variant build with output_root
    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern((fixture.root() / "build").string()),
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    // Set build root name for variant build (normally done by builder.build())
    set_build_root_name(bs, "build");

    ctx.tup_variant_outputdir = intern("../../build/src/lib");

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src/lib"));

    // Rule: : add.c |> gcc -c %f -o %o |> add.o
    auto rule = Rule {};
    rule.inputs.push_back(make_path_pattern("add.c"));
    rule.command.parts.push_back(Expression::Literal { intern("gcc -c %f -o %o") });
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("add.o") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    auto commands = nodes_of_type(bs.graph,NodeType::Command);
    REQUIRE(!commands.empty());

    auto cmd_str = expand_instruction(bs.graph, commands[0]);
    INFO("Command: " << std::string { sv(cmd_str) });

    // Input should be simplified (just "add.c", not round-trip path)
    CHECK(sv(cmd_str).find("-c add.c") != std::string_view::npos);

    // Output should go to variant directory
    CHECK(sv(cmd_str).find("build/src/lib/add.o") != std::string_view::npos);
}

// =============================================================================
// Dotdot filename edge cases (Issue #1: fragile ".." detection)
// =============================================================================

TEST_CASE("GraphBuilder output filename starting with dotdot is not parent reference", "[e2e][builder][paths][dotdot]")
{
    // Regression test: filenames like "..hidden" must be treated as literal filenames,
    // not as parent directory references. Node traversal handles this correctly because
    // only a path component equal to ".." is treated as parent traversal.

    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/input.c");
    fixture.create_file("src/Tupfile");
    fs::create_directories(fixture.root() / "build" / "src");

    auto bs = make_build_graph();
    set_build_root_name(bs, "build");
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = intern((fixture.root() / "build").string()),
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    ctx.tup_variant_outputdir = intern("../../build/src");

    auto tupfile = Tupfile {};
    tupfile.filename = intern(fixture.tupfile_path("src"));

    // Rule that outputs a file named "..hidden" (valid filename starting with ..)
    auto rule = Rule {};
    rule.inputs.push_back(make_path_pattern("input.c"));
    rule.command.parts.push_back(Expression::Literal { intern("gen-hidden %f %o") });
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { intern("..hidden") });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = add_tupfile(bs, tupfile, ctx, builder_state);
    REQUIRE(result.has_value());

    // Find the output node
    auto generated = nodes_of_type(bs.graph,NodeType::Generated);
    NodeId output_id = 0;
    for (auto id : generated) {
        if (sv(get<Name>(bs.graph, id)) == "..hidden") {
            output_id = id;
            break;
        }
    }
    REQUIRE(output_id != 0);

    // In variant build, output goes to build/src/..hidden
    // The key check is that "..hidden" is treated as a literal filename, not "../hidden"
    auto full_path = get_full_path(bs.graph, output_id);
    INFO("Full path: " << std::string { sv(full_path) });
    CHECK(sv(full_path) == "build/src/..hidden");
}

namespace {

auto add_tupfile_from_source(
    BuilderTestFixture const& fixture,
    std::string_view source,
    VarDb const* config_vars = nullptr
) -> pup::Result<void>
{
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars, .config_vars = config_vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto parse_result = parse_tupfile(source, fixture.tupfile_path(""));
    REQUIRE(parse_result.success());

    return add_tupfile(bs, parse_result.tupfile, ctx, builder_state);
}

} // namespace

TEST_CASE("GraphBuilder error directive fails with file and line", "[builder][error-directive]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(fixture, "error boom\n");

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("boom") != std::string_view::npos);
    CHECK(msg.find("Tupfile:1") != std::string_view::npos);
}

TEST_CASE("GraphBuilder error directive in inactive branch is skipped", "[builder][error-directive]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(fixture, "ifeq (a,b)\nerror boom\nendif\n");

    REQUIRE(result.has_value());
}

TEST_CASE("GraphBuilder error directive in active branch fails with expanded message", "[builder][error-directive]")
{
    auto fixture = BuilderTestFixture {};
    auto config = VarDb {};
    config.set("BROKEN", "y");
    config.set("TOOLCHAIN", "arm");

    auto result = add_tupfile_from_source(
        fixture,
        "V = world\nifeq (@(BROKEN),y)\nerror hello $(V) @(TOOLCHAIN)\nendif\n",
        &config
    );

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("hello world arm") != std::string_view::npos);
    CHECK(msg.find("Tupfile:3") != std::string_view::npos);
}

TEST_CASE("GraphBuilder error directive in active branch nested inside inactive branch is skipped", "[builder][error-directive]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(
        fixture,
        "ifeq (a,b)\nifeq (y,y)\nerror boom\nendif\nendif\n"
    );

    REQUIRE(result.has_value());
}

TEST_CASE("GraphBuilder bare error directive reports empty error directive", "[builder][error-directive]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(fixture, "error\n");

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("Empty error directive") != std::string_view::npos);
}

TEST_CASE("GraphBuilder error directive expanding to empty keeps the empty message", "[builder][error-directive]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(fixture, "error $(UNSET)\n");

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("Empty error directive") == std::string_view::npos);
    CHECK(msg.find("Tupfile:1") != std::string_view::npos);
}

TEST_CASE("GraphBuilder rejects command expanding to empty and names the config vars read", "[builder][empty-command]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(fixture, ": |> @(CC) |> out.o\n");

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("Tupfile:1") != std::string_view::npos);
    CHECK(msg.find("empty") != std::string_view::npos);
    CHECK(msg.find("@(CC)") != std::string_view::npos);
}

TEST_CASE("GraphBuilder rejects command expanding to whitespace only", "[builder][empty-command]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(fixture, ": |> @(CC) @(CFLAGS) |> out.o\n");

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("Tupfile:1") != std::string_view::npos);
    CHECK(msg.find("@(CC)") != std::string_view::npos);
    CHECK(msg.find("@(CFLAGS)") != std::string_view::npos);
}

TEST_CASE("GraphBuilder rejects empty command without config hint when no config vars read", "[builder][empty-command]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(fixture, ": |> $(UNSET) |> out.o\n");

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("Tupfile:1") != std::string_view::npos);
    CHECK(msg.find("empty") != std::string_view::npos);
    CHECK(msg.find("tup.config") == std::string_view::npos);
}

TEST_CASE("GraphBuilder accepts empty-rendered command in inactive config branch", "[builder][empty-command]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(
        fixture,
        "ifeq (@(TESTS),y)\n: |> @(CC) |> out.o\nendif\n"
    );

    REQUIRE(result.has_value());
}

TEST_CASE("GraphBuilder rejects empty-rendered command in active config branch", "[builder][empty-command]")
{
    auto fixture = BuilderTestFixture {};
    auto config = VarDb {};
    config.set("TESTS", "y");

    auto result = add_tupfile_from_source(
        fixture,
        "ifeq (@(TESTS),y)\n: |> @(CC) |> out.o\nendif\n",
        &config
    );

    REQUIRE_FALSE(result.has_value());
    auto msg = result.error().msg();
    CHECK(msg.find("Tupfile:2") != std::string_view::npos);
    CHECK(msg.find("@(CC)") != std::string_view::npos);
}

TEST_CASE("GraphBuilder accepts command that renders nonempty from config var", "[builder][empty-command]")
{
    auto fixture = BuilderTestFixture {};
    auto config = VarDb {};
    config.set("CC", "gcc");

    auto result = add_tupfile_from_source(fixture, ": |> @(CC) |> out.o\n", &config);

    REQUIRE(result.has_value());
}

TEST_CASE("GraphBuilder statically false branch does not claim outputs", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};

    auto result = add_tupfile_from_source(
        fixture,
        "ifeq (a,b)\n: |> echo gen > %o |> out.txt\nendif\n: |> echo gen > %o |> out.txt\n"
    );

    REQUIRE(result.has_value());
}

namespace {

auto count_conditions(
    BuilderTestFixture const& fixture,
    std::string_view source,
    VarDb const* config_vars = nullptr
) -> std::size_t
{
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars, .config_vars = config_vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto parse_result = parse_tupfile(source, fixture.tupfile_path(""));
    REQUIRE(parse_result.success());
    REQUIRE(add_tupfile(bs, parse_result.tupfile, ctx, builder_state).has_value());
    return bs.graph.conditions.size();
}

} // namespace

TEST_CASE("GraphBuilder static conditional adds no condition node", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};

    auto wrapped = count_conditions(fixture, "ifeq (y,y)\n: |> echo gen > %o |> out.txt\nendif\n");
    auto plain = count_conditions(fixture, ": |> echo gen > %o |> out.txt\n");

    CHECK(wrapped == plain);
}

TEST_CASE("GraphBuilder config conditional still creates a condition node", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};

    auto wrapped = count_conditions(fixture, "ifeq (@(M),y)\n: |> echo gen > %o |> out.txt\nendif\n");
    auto plain = count_conditions(fixture, ": |> echo gen > %o |> out.txt\n");

    CHECK(wrapped > plain);
}

TEST_CASE("GraphBuilder platform conditional is dynamic without env override", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};

    auto wrapped = count_conditions(
        fixture,
        "ifeq ($(TUP_PLATFORM),__pup_nonexistent__)\n: |> echo gen > %o |> out.txt\nendif\n"
    );
    auto plain = count_conditions(fixture, ": |> echo gen > %o |> out.txt\n");

    CHECK(wrapped > plain);
}

TEST_CASE("GraphBuilder inactive branch rules expand branch-local assignments", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};
    auto bs = make_build_graph();
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = intern(fixture.root_str()),
        .config_root = intern(fixture.root_str()),
        .output_root = pup::StringId::Empty,
        .config_path = pup::StringId::Empty,
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder_state = make_builder(options);

    auto parse_result = parse_tupfile(
        "ifeq (@(X),y)\nV = branchval\n: |> gen $(V) > %o |> out.txt\nendif\n",
        fixture.tupfile_path("")
    );
    REQUIRE(parse_result.success());
    REQUIRE(add_tupfile(bs, parse_result.tupfile, ctx, builder_state).has_value());

    auto found = false;
    for (std::size_t i = 0; i < bs.graph.commands.size(); ++i) {
        auto instruction = sv(bs.graph.commands[i].instruction_id);
        if (instruction.find("branchval") != std::string_view::npos) {
            found = true;
        }
    }
    CHECK(found);
    CHECK_FALSE(vars.contains("V"));
}

TEST_CASE("GraphBuilder ifdef on a config var creates a condition node", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};

    auto wrapped = count_conditions(fixture, "ifdef M\n: |> echo gen > %o |> out.txt\nendif\n");
    auto plain = count_conditions(fixture, ": |> echo gen > %o |> out.txt\n");

    CHECK(wrapped > plain);
}

TEST_CASE("GraphBuilder ifdef on a plain variable is textual", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};

    auto wrapped = count_conditions(fixture, "V = 1\nifdef V\n: |> echo gen > %o |> out.txt\nendif\n");
    auto plain = count_conditions(fixture, "V = 1\n: |> echo gen > %o |> out.txt\n");

    CHECK(wrapped == plain);
}

TEST_CASE("GraphBuilder variable assigned under a config branch keeps later conditions dynamic", "[builder][conditional]")
{
    auto fixture = BuilderTestFixture {};
    auto config = VarDb {};
    config.set("A", "y");

    auto wrapped = count_conditions(
        fixture,
        "ifeq (@(A),y)\nV = 1\nendif\nifeq ($(V),1)\n: |> echo gen > %o |> out.txt\nendif\n",
        &config
    );
    auto baseline = count_conditions(fixture, "ifeq (@(A),y)\nV = 1\nendif\n", &config);

    CHECK(wrapped > baseline);
}
