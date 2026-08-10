// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "options.hpp"
#include "pup/core/function.hpp"
#include "pup/core/result.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/parser/ast.hpp"
#include "pup/parser/ignore.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace pup {
struct ProjectLayout;
struct LayoutOptions;
namespace parser {
class VarDb;
}
namespace graph {
struct BuildGraph;
class RulePatternRegistry;
}
namespace index {
class Index;
}
}

namespace pup::cli {

/// Callback type for statement inspection (strict checking)
using StatementCallback = Function<void(parser::Statement const&, std::string_view dir)>;

/// Callback type for tracking variable assignments
using VarAssignedCallback = Function<void(
    std::string_view name,
    parser::Assignment::Op op,
    std::string_view value_before,
    std::string_view value_after,
    std::string_view filename,
    std::uint32_t line,
    std::uint32_t column,
    bool is_effective
)>;

/// Options for building the dependency graph
struct BuildContextOptions {
    bool verbose = false;
    bool keep_going = false;
    bool auto_init = false;
    bool dry_run = false;
    bool root_config_only = false;
    bool require_config = false;
    Vec<StringId> parse_scopes = {};
    parser::IgnoreList excludes = {};
    graph::DepScannerRegistry* scanner_registry = nullptr;
    graph::RulePatternRegistry* pattern_registry = nullptr;
    VarAssignedCallback on_var_assigned = {};
    StatementCallback on_statement = {};
};

/// Create scanner registry for implicit dependency tracking
/// Returns nullopt if PUP_IMPLICIT_DEPS=0
[[nodiscard]]
auto make_scanner_registry() -> std::optional<graph::DepScannerRegistry>;

/// Build context using PIMPL to hide heavy dependencies
class BuildContext final {
public:
    BuildContext();
    ~BuildContext();

    BuildContext(BuildContext const&) = delete;
    auto operator=(BuildContext const&) -> BuildContext& = delete;

    BuildContext(BuildContext&&) noexcept;
    auto operator=(BuildContext&&) noexcept -> BuildContext&;

    [[nodiscard]]
    auto layout() const -> ProjectLayout const&;
    [[nodiscard]]
    auto graph() const -> graph::BuildGraph const&;
    [[nodiscard]]
    auto graph() -> graph::BuildGraph&;
    /// Root-level config vars (from output_root/tup.config + -D overrides).
    /// Per-directory Tupfile evaluation uses scoped merged configs instead.
    [[nodiscard]]
    auto config_vars() const -> parser::VarDb const&;
    [[nodiscard]]
    auto vars() const -> parser::VarDb const&;
    [[nodiscard]]
    auto parsed_dirs() const -> Vec<StringId> const&;
    /// Directories discovered to contain a Tupfile this run, whether or not the
    /// parse of that Tupfile succeeded. A dir in this set but not parsed_dirs()
    /// failed to parse or was skipped by the scope filter.
    [[nodiscard]]
    auto available_dirs() const -> Vec<StringId> const&;
    /// Nested-project roots pruned from discovery this run: subtrees this build
    /// has no authority over, so their commands and outputs must be preserved.
    [[nodiscard]]
    auto pruned_dirs() const -> Vec<StringId> const&;

    /// Get the old index loaded from disk (if any)
    /// Returns nullptr if no index exists or failed to load
    [[nodiscard]]
    auto old_index() const -> index::Index const*;

    /// Paths the previous record classified as outputs and that this build has no authority to
    /// reclassify, sorted by handle. Empty for a full build, which parses every producer and so
    /// answers for itself (#369).
    [[nodiscard]]
    auto prior_generated() const -> Vec<StringId> const&;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;

    friend auto build_context(
        Options const&,
        BuildContextOptions const&
    ) -> Result<BuildContext>;
};

/// Build the dependency graph from Tupfiles
[[nodiscard]]
auto build_context(
    Options const& opts,
    BuildContextOptions const& ctx_opts = {}
) -> Result<BuildContext>;

/// Compute build scopes from cwd relative to source_root.
/// Returns empty vector for full project build, or path prefixes for scoped build.
[[nodiscard]]
auto compute_build_scopes(
    Options const& opts,
    ProjectLayout const& layout
) -> Vec<StringId>;

/// Build the -x exclude patterns into an IgnoreList
[[nodiscard]]
auto make_exclude_list(Options const& opts) -> parser::IgnoreList;

/// Convert CLI options to layout discovery options
[[nodiscard]]
auto make_layout_options(Options const& opts) -> LayoutOptions;

/// Context for clean commands
struct CleanContext {
    StringId root = StringId::Empty;
    StringId build_dir = StringId::Empty;
    bool is_in_tree;
};

/// Resolve clean context from options
[[nodiscard]]
auto resolve_clean_context(
    Options const& opts
) -> std::optional<CleanContext>;

} // namespace pup::cli
