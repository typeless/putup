// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "dag.hpp"
#include "pup/core/result.hpp"
#include "pup/parser/ast.hpp"
#include "pup/parser/eval.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pup::graph {

/// Options for graph building
struct BuilderOptions {
    std::filesystem::path root_dir;     ///< Project root directory
    bool expand_globs = true;           ///< Expand glob patterns
    bool validate_inputs = true;        ///< Check that input files exist
    bool verbose = false;               ///< Print verbose output
};

/// Bang macro definition
struct BangMacroDef {
    std::string name;
    bool foreach_ = false;
    std::vector<parser::PathPattern> order_only_inputs;
    parser::Expression command;
    std::optional<parser::Expression> display;
    std::vector<parser::PathPattern> outputs;
    std::vector<parser::PathPattern> extra_outputs;
};

/// Context for building the graph
struct BuilderContext {
    BuildGraph* graph = nullptr;
    parser::EvalContext* eval = nullptr;
    BuilderOptions options = {};

    std::unordered_map<std::string, BangMacroDef> macros = {};
    std::unordered_map<std::string, std::vector<NodeId>> groups = {};

    std::filesystem::path current_dir = {};
    std::string current_file = {};

    std::vector<std::string> errors = {};
    std::vector<std::string> warnings = {};
};

/// Build graph from a parsed Tupfile
class GraphBuilder {
public:
    explicit GraphBuilder(BuilderOptions options = {});

    /// Build graph from a single Tupfile AST
    [[nodiscard]] auto build(parser::Tupfile const& tupfile, parser::EvalContext& eval)
        -> Result<BuildGraph>;

    /// Add a Tupfile to an existing graph
    [[nodiscard]] auto add_tupfile(
        BuildGraph& graph,
        parser::Tupfile const& tupfile,
        parser::EvalContext& eval) -> Result<void>;

    /// Get build errors
    [[nodiscard]] auto errors() const -> std::vector<std::string> const& { return errors_; }

    /// Get build warnings
    [[nodiscard]] auto warnings() const -> std::vector<std::string> const& { return warnings_; }

private:
    BuilderOptions options_;
    std::vector<std::string> errors_;
    std::vector<std::string> warnings_;

    auto process_statement(
        BuilderContext& ctx,
        parser::Statement const& stmt) -> Result<void>;

    auto process_rule(
        BuilderContext& ctx,
        parser::Rule const& rule) -> Result<void>;

    auto process_bang_macro(
        BuilderContext& ctx,
        parser::BangMacro const& macro) -> Result<void>;

    auto process_assignment(
        BuilderContext& ctx,
        parser::Assignment const& assign) -> Result<void>;

    auto process_conditional(
        BuilderContext& ctx,
        parser::Conditional const& cond) -> Result<void>;

    auto expand_rule(
        BuilderContext& ctx,
        parser::Rule const& rule,
        std::vector<std::string> const& inputs) -> Result<void>;

    auto expand_inputs(
        BuilderContext& ctx,
        std::vector<parser::PathPattern> const& patterns) -> Result<std::vector<std::string>>;

    auto expand_outputs(
        BuilderContext& ctx,
        std::vector<parser::PathPattern> const& patterns,
        std::string const& input) -> Result<std::vector<std::string>>;

    auto expand_command(
        BuilderContext& ctx,
        parser::Expression const& cmd,
        std::vector<std::string> const& inputs,
        std::vector<std::string> const& outputs) -> Result<std::string>;

    auto get_or_create_file_node(
        BuilderContext& ctx,
        std::string const& path,
        NodeType type = NodeType::File) -> Result<NodeId>;

    auto create_command_node(
        BuilderContext& ctx,
        std::string const& command,
        std::string const& display) -> Result<NodeId>;
};

} // namespace pup::graph
