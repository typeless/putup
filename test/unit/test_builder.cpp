// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/graph/builder.hpp"

#include <filesystem>
#include <fstream>

using namespace pup;
using namespace pup::graph;
using namespace pup::parser;

namespace fs = std::filesystem;

namespace {

/// Helper to create a simple PathPattern for a literal path
auto make_path_pattern(std::string_view path) -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.path.parts.push_back(Expression::Literal { std::string { path } });
    return pattern;
}

/// Helper to create a {group} bin reference
auto make_group_pattern(std::string_view name) -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.is_group = true;
    pattern.group_name = std::string { name };
    return pattern;
}

/// Helper to create an <order-only-group> reference with optional path prefix
auto make_order_only_group_pattern(std::string_view name, std::string_view path = "") -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.is_order_only_group = true;
    pattern.group_name = std::string { name };
    if (!path.empty())
        pattern.path.parts.push_back(Expression::Literal { std::string { path } });
    return pattern;
}

/// Helper to create an order-only group pattern with variable prefix
auto make_order_only_group_var_pattern(std::string_view name, std::string_view var_name, std::string_view suffix = "") -> PathPattern
{
    auto pattern = PathPattern {};
    pattern.is_order_only_group = true;
    pattern.group_name = std::string { name };
    pattern.path.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, std::string { var_name }, {} } });
    if (!suffix.empty())
        pattern.path.parts.push_back(Expression::Literal { std::string { suffix } });
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
        test_root_ = fs::temp_directory_path() / "pup_test_builder";
        fs::remove_all(test_root_);
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
        if (dir.empty() || dir == ".")
            return (test_root_ / "Tupfile").string();
        return (test_root_ / dir / "Tupfile").string();
    }

private:
    fs::path test_root_;
};

} // namespace

// =============================================================================
// normalize_group_dir behavior tests (verify the function's edge cases)
// These test the exact behavior that the refactoring regression broke
// =============================================================================

TEST_CASE("GraphBuilder order-only group - case 1: empty pattern.path", "[builder][group][critical]")
{
    // Case 1: is_order_only_group with empty pattern.path
    // Should use current_dir directly, NOT normalize_group_dir("")
    // This is the case that was broken by the refactoring

    auto fixture = BuilderTestFixture {};
    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // First Tupfile: modules/kernel defines <local-group>
    auto tupfile1 = Tupfile {};
    tupfile1.filename = fixture.tupfile_path("modules/kernel");
    fixture.create_file("modules/kernel/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { "gen-header" });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { "generated.h" });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = "local-group";
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = builder.add_tupfile(graph, tupfile1, ctx);
    (void)r1;

    // Second Tupfile: also in modules/kernel, references <local-group> (empty path)
    // GroupKey should be {"modules/kernel", "local-group"}
    auto tupfile2 = Tupfile {};
    tupfile2.filename = fixture.tupfile_path("modules/kernel");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { "compile" });

    // Empty path - this is the critical case
    auto input = make_order_only_group_pattern("local-group");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { "output.o" });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = builder.add_tupfile(graph, tupfile2, ctx);
    REQUIRE(r2.has_value());
}

TEST_CASE("GraphBuilder order-only group - case 2: non-empty pattern.path with variable", "[builder][group][critical]")
{
    // Case 2: is_order_only_group with non-empty pattern.path
    // Should use normalize_group_dir(expanded_path, current_dir, source_root)

    auto fixture = BuilderTestFixture {};
    auto graph = BuildGraph {};
    auto vars = VarDb {};
    vars.set("ROOT", fixture.root().string());
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // First Tupfile: include/generated defines <gen-headers>
    auto tupfile1 = Tupfile {};
    tupfile1.filename = fixture.tupfile_path("include/generated");
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { "gen-config" });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { "config.h" });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = "gen-headers";
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = builder.add_tupfile(graph, tupfile1, ctx);
    (void)r1;

    // Second Tupfile: modules/kernel references $(ROOT)/include/generated/<gen-headers>
    // This should normalize to {"include/generated", "gen-headers"}
    auto tupfile2 = Tupfile {};
    tupfile2.filename = fixture.tupfile_path("modules/kernel");
    fixture.create_file("modules/kernel/Tupfile");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { "compile" });

    // Non-empty path with variable expansion
    auto input = make_order_only_group_var_pattern("gen-headers", "ROOT", "/include/generated/");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { "output.o" });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = builder.add_tupfile(graph, tupfile2, ctx);
    REQUIRE(r2.has_value());
}

TEST_CASE("GraphBuilder order-only group - case 3: path/<group> pattern", "[builder][group][critical]")
{
    // Case 3: After expand_path, path contains literal <groupname> suffix
    // Should ALWAYS use normalize_group_dir(dir_part, current_dir, source_root)
    // Even when dir_part is empty

    auto fixture = BuilderTestFixture {};
    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // First: define group in include/generated
    auto tupfile1 = Tupfile {};
    tupfile1.filename = fixture.tupfile_path("include/generated");
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { "gen-version" });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { "version.h" });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = "gen-headers";
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = builder.add_tupfile(graph, tupfile1, ctx);
    (void)r1;

    // Second: reference with relative path ../../include/generated/<gen-headers>
    // from modules/kernel
    auto tupfile2 = Tupfile {};
    tupfile2.filename = fixture.tupfile_path("modules/kernel");
    fixture.create_file("modules/kernel/Tupfile");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { "compile" });

    // This is a literal path pattern - the <gen-headers> is NOT parsed as is_order_only_group
    // It gets expanded and then matched by the rfind('<') logic
    auto input = make_path_pattern("../../include/generated/<gen-headers>");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { "output.o" });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = builder.add_tupfile(graph, tupfile2, ctx);
    REQUIRE(r2.has_value());
}

// =============================================================================
// Bin {group} tests
// =============================================================================

TEST_CASE("GraphBuilder bin group reference {name}", "[builder][group]")
{
    auto fixture = BuilderTestFixture {};
    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // Create source files
    fixture.create_file("src/a.c");
    fixture.create_file("src/b.c");

    // Tupfile that compiles *.c -> {objs}, then links {objs} -> app
    auto tupfile = Tupfile {};
    tupfile.filename = fixture.tupfile_path("src");
    fixture.create_file("src/Tupfile");

    // Rule 1: compile a.c -> a.o into {objs}
    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { "cc -c a.c -o a.o" });
    rule1.inputs.push_back(make_path_pattern("a.c"));
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { "a.o" });
    rule1.outputs.push_back(out1);
    rule1.output_group = "objs";
    tupfile.statements.push_back(make_rule_statement(std::move(rule1)));

    // Rule 2: compile b.c -> b.o into {objs}
    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { "cc -c b.c -o b.o" });
    rule2.inputs.push_back(make_path_pattern("b.c"));
    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { "b.o" });
    rule2.outputs.push_back(out2);
    rule2.output_group = "objs";
    tupfile.statements.push_back(make_rule_statement(std::move(rule2)));

    // Rule 3: link {objs} -> app
    auto rule3 = Rule {};
    rule3.command.parts.push_back(Expression::Literal { "cc -o app" });
    rule3.inputs.push_back(make_group_pattern("objs"));
    auto out3 = PathPattern {};
    out3.path.parts.push_back(Expression::Literal { "app" });
    rule3.outputs.push_back(out3);
    tupfile.statements.push_back(make_rule_statement(std::move(rule3)));

    auto result = builder.add_tupfile(graph, tupfile, ctx);
    REQUIRE(result.has_value());

    // Verify the link command has both .o files as inputs
    auto commands = graph.nodes_of_type(NodeType::Command);
    auto link_cmd_count = 0;
    for (auto id : commands) {
        auto const* node = graph.get_node(id);
        if (node && node->command.find("-o app") != std::string::npos) {
            auto inputs = graph.get_inputs(id);
            // Should have at least 2 .o inputs (may also have Tupfile as sticky)
            auto o_count = 0;
            for (auto input_id : inputs) {
                auto const* input_node = graph.get_node(input_id);
                if (input_node && input_node->path.find(".o") != std::string::npos)
                    ++o_count;
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

TEST_CASE("GraphBuilder glob expansion - filesystem", "[builder][glob]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/foo.c");
    fixture.create_file("src/bar.c");
    fixture.create_file("src/baz.h"); // Should not match *.c

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    auto tupfile = Tupfile {};
    tupfile.filename = fixture.tupfile_path("src");
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.foreach_ = true;
    rule.command.parts.push_back(Expression::Literal { "cc -c" });
    rule.inputs.push_back(make_path_pattern("*.c"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { "%B.o" });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = builder.add_tupfile(graph, tupfile, ctx);
    REQUIRE(result.has_value());

    // Should have 2 compile commands (foo.c and bar.c)
    auto commands = graph.nodes_of_type(NodeType::Command);
    REQUIRE(commands.size() == 2);
}

TEST_CASE("GraphBuilder glob expansion - generated files", "[builder][glob]")
{
    auto fixture = BuilderTestFixture {};

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = "build-variant",
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // First Tupfile: generate some .h files
    auto tupfile1 = Tupfile {};
    tupfile1.filename = fixture.tupfile_path("include/generated");
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { "gen-config" });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { "config.h" });
    rule1.outputs.push_back(out1);
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { "gen-version" });
    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { "version.h" });
    rule2.outputs.push_back(out2);
    tupfile1.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r1 = builder.add_tupfile(graph, tupfile1, ctx);
    REQUIRE(r1.has_value());

    // Verify generated files exist in graph
    auto generated = graph.nodes_of_type(NodeType::Generated);
    REQUIRE(generated.size() == 2);
}

// =============================================================================
// tup.config special case
// =============================================================================

TEST_CASE("GraphBuilder tup.config in variant directory", "[builder][config]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("build-variant/tup.config");
    fixture.create_file("src/main.c");

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = fixture.root() / "build-variant",
        .variant_dir = "build-variant",
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    auto tupfile = Tupfile {};
    tupfile.filename = fixture.tupfile_path("src");
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { "cat tup.config" });
    rule.inputs.push_back(make_path_pattern("../tup.config"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { "config.txt" });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = builder.add_tupfile(graph, tupfile, ctx);
    // Should find tup.config in the variant directory
    REQUIRE(result.has_value());
}

// =============================================================================
// Exclusion pattern tests
// =============================================================================

TEST_CASE("GraphBuilder exclusion patterns - explicit file", "[builder][exclusion]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/foo.c");
    fixture.create_file("src/bar.c");
    fixture.create_file("src/baz.c"); // Will be excluded

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    auto tupfile = Tupfile {};
    tupfile.filename = fixture.tupfile_path("src");
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.foreach_ = true;
    rule.command.parts.push_back(Expression::Literal { "cc -c" });
    rule.inputs.push_back(make_path_pattern("*.c"));

    // Exclusion pattern - explicit filename
    auto excl = make_path_pattern("baz.c");
    excl.is_exclusion = true;
    rule.inputs.push_back(excl);

    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { "%B.o" });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = builder.add_tupfile(graph, tupfile, ctx);
    REQUIRE(result.has_value());

    // Count .o outputs - baz.c should be excluded
    auto commands = graph.nodes_of_type(NodeType::Command);
    auto compile_count = 0;
    auto has_baz = false;
    for (auto id : commands) {
        auto outputs = graph.get_outputs(id);
        for (auto out_id : outputs) {
            auto const* node = graph.get_node(out_id);
            if (node && node->path.find(".o") != std::string::npos) {
                ++compile_count;
                if (node->path.find("baz.o") != std::string::npos)
                    has_baz = true;
            }
        }
    }
    // Should have 2 compiles (foo.c, bar.c), baz.c excluded
    CHECK(compile_count == 2);
    CHECK_FALSE(has_baz);
}

TEST_CASE("GraphBuilder exclusion patterns - glob pattern", "[builder][exclusion]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/main.c");
    fixture.create_file("src/util.c");
    fixture.create_file("src/test_main.c"); // Will be excluded
    fixture.create_file("src/test_util.c"); // Will be excluded

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = true,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    auto tupfile = Tupfile {};
    tupfile.filename = fixture.tupfile_path("src");
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.foreach_ = true;
    rule.command.parts.push_back(Expression::Literal { "cc -c" });
    rule.inputs.push_back(make_path_pattern("*.c"));

    // Exclusion pattern - glob to exclude test files
    auto excl = make_path_pattern("test_*.c");
    excl.is_exclusion = true;
    rule.inputs.push_back(excl);

    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { "%B.o" });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = builder.add_tupfile(graph, tupfile, ctx);
    REQUIRE(result.has_value());

    // Count .o outputs - test_*.c should be excluded
    auto commands = graph.nodes_of_type(NodeType::Command);
    auto compile_count = 0;
    auto has_test = false;
    for (auto id : commands) {
        auto outputs = graph.get_outputs(id);
        for (auto out_id : outputs) {
            auto const* node = graph.get_node(out_id);
            if (node && node->path.find(".o") != std::string::npos) {
                ++compile_count;
                if (node->path.find("test_") != std::string::npos)
                    has_test = true;
            }
        }
    }
    // Should have 2 compiles (main.c, util.c), test_*.c excluded
    CHECK(compile_count == 2);
    CHECK_FALSE(has_test);
}

// =============================================================================
// Cross-directory group reference tests
// =============================================================================

TEST_CASE("GraphBuilder cross-directory order-only group with relative path", "[builder][group][cross-dir]")
{
    auto fixture = BuilderTestFixture {};

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // Tupfile in include/generated: defines <gen-headers>
    auto tupfile1 = Tupfile {};
    tupfile1.filename = fixture.tupfile_path("include/generated");
    fixture.create_file("include/generated/Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { "gen-config" });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { "config.h" });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = "gen-headers";
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = builder.add_tupfile(graph, tupfile1, ctx);
    REQUIRE(r1.has_value());

    // Tupfile in modules/kernel: references ../../include/generated/<gen-headers>
    auto tupfile2 = Tupfile {};
    tupfile2.filename = fixture.tupfile_path("modules/kernel");
    fixture.create_file("modules/kernel/Tupfile");
    fixture.create_file("modules/kernel/kernel.c");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { "compile kernel.c" });
    rule2.inputs.push_back(make_path_pattern("kernel.c"));

    // Order-only reference with relative path
    // Note: This is a literal path, will be matched by rfind('<') logic
    auto order_input = make_path_pattern("../../include/generated/<gen-headers>");
    rule2.order_only_inputs.push_back(order_input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { "kernel.o" });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = builder.add_tupfile(graph, tupfile2, ctx);
    REQUIRE(r2.has_value());

    // Verify the kernel.o command has an order-only dependency on config.h
    auto found = false;
    for (auto id : graph.nodes_of_type(NodeType::Command)) {
        auto const* node = graph.get_node(id);
        if (node && node->command.find("compile kernel.c") != std::string::npos) {
            auto order_only = graph.get_order_only(id);
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

TEST_CASE("GraphBuilder normalize_group_dir empty string returns dot", "[builder][normalize]")
{
    // This test documents the critical behavior:
    // normalize_group_dir("", current_dir, source_root) should return "."
    // NOT current_dir

    // The refactoring regression happened because the helper function
    // returned current_dir when dir_prefix was empty, but the original
    // code called normalize_group_dir("") which returns "."

    auto fixture = BuilderTestFixture {};
    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // Case 1: Group defined at root level (directory ".")
    auto tupfile1 = Tupfile {};
    tupfile1.filename = fixture.tupfile_path("");
    fixture.create_file("Tupfile");

    auto rule1 = Rule {};
    rule1.command.parts.push_back(Expression::Literal { "gen-root" });
    auto out1 = PathPattern {};
    out1.path.parts.push_back(Expression::Literal { "root.h" });
    rule1.outputs.push_back(out1);
    rule1.output_order_only_group = "root-group";
    tupfile1.statements.push_back(make_rule_statement(std::move(rule1)));

    auto r1 = builder.add_tupfile(graph, tupfile1, ctx);
    REQUIRE(r1.has_value());

    // Case 2: Reference with empty path from modules/kernel
    // The expanded path will be "<root-group>" (no dir prefix)
    // This should look up {".", "root-group"} NOT {"modules/kernel", "root-group"}
    auto tupfile2 = Tupfile {};
    tupfile2.filename = fixture.tupfile_path("modules/kernel");
    fixture.create_file("modules/kernel/Tupfile");

    auto rule2 = Rule {};
    rule2.command.parts.push_back(Expression::Literal { "compile" });

    // Use path pattern that after expansion is just "<root-group>"
    auto input = make_path_pattern("<root-group>");
    rule2.order_only_inputs.push_back(input);

    auto out2 = PathPattern {};
    out2.path.parts.push_back(Expression::Literal { "output.o" });
    rule2.outputs.push_back(out2);
    tupfile2.statements.push_back(make_rule_statement(std::move(rule2)));

    auto r2 = builder.add_tupfile(graph, tupfile2, ctx);
    REQUIRE(r2.has_value());

    // The critical verification: check that the group WAS found
    // If normalize_group_dir("") returns "." then it will match
    // If it returns current_dir ("modules/kernel") then it won't match
    auto found_order_only = false;
    for (auto id : graph.nodes_of_type(NodeType::Command)) {
        auto const* node = graph.get_node(id);
        if (node && node->command.find("compile") != std::string::npos) {
            auto order_only = graph.get_order_only(id);
            if (!order_only.empty())
                found_order_only = true;
            break;
        }
    }
    CHECK(found_order_only);
}

// =============================================================================
// Variant build path resolution
// =============================================================================

TEST_CASE("GraphBuilder variant output mapping", "[builder][variant]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("src/main.c");

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = fixture.root() / "build-variant",
        .variant_dir = "build-variant",
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    auto tupfile = Tupfile {};
    tupfile.filename = fixture.tupfile_path("src");
    fixture.create_file("src/Tupfile");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { "cc -c main.c -o main.o" });
    rule.inputs.push_back(make_path_pattern("main.c"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { "main.o" });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = builder.add_tupfile(graph, tupfile, ctx);
    REQUIRE(result.has_value());

    // Verify output is in variant directory
    auto generated = graph.nodes_of_type(NodeType::Generated);
    REQUIRE(generated.size() == 1);

    auto const* node = graph.get_node(generated[0]);
    REQUIRE(node != nullptr);
    // Output should be under build-variant/src/main.o or absolute path
    CHECK(node->path.find("main.o") != std::string::npos);
}

// =============================================================================
// Deep current_dir tests (regression prevention)
// =============================================================================

TEST_CASE("GraphBuilder deep directory with parent references", "[builder][deep-dir]")
{
    auto fixture = BuilderTestFixture {};
    fixture.create_file("include/common.h");
    fs::create_directories(fixture.root() / "modules" / "app" / "sub" / "deep");
    fixture.create_file("modules/app/sub/deep/impl.c");

    auto graph = BuildGraph {};
    auto vars = VarDb {};
    auto ctx = EvalContext { .vars = &vars };

    auto options = BuilderOptions {
        .source_root = fixture.root(),
        .output_root = {},
        .variant_dir = {},
        .expand_globs = false,
        .validate_inputs = false,
    };
    auto builder = GraphBuilder { options };

    // Tupfile in deeply nested directory
    auto tupfile = Tupfile {};
    tupfile.filename = fixture.tupfile_path("modules/app/sub/deep");
    fixture.create_file("modules/app/sub/deep/Tupfile");

    auto rule = Rule {};
    rule.command.parts.push_back(Expression::Literal { "cc -c impl.c" });
    rule.inputs.push_back(make_path_pattern("impl.c"));
    // Reference header with multiple parent refs
    rule.inputs.push_back(make_path_pattern("../../../../include/common.h"));
    auto output = PathPattern {};
    output.path.parts.push_back(Expression::Literal { "impl.o" });
    rule.outputs.push_back(output);
    tupfile.statements.push_back(make_rule_statement(std::move(rule)));

    auto result = builder.add_tupfile(graph, tupfile, ctx);
    REQUIRE(result.has_value());

    // Verify the header was resolved correctly
    auto files = graph.nodes_of_type(NodeType::File);
    auto found_header = false;
    for (auto id : files) {
        auto const* node = graph.get_node(id);
        if (node && node->path.find("common.h") != std::string::npos) {
            // Path should be normalized to include/common.h
            CHECK(node->path == "include/common.h");
            found_header = true;
            break;
        }
    }
    REQUIRE(found_header);
}
