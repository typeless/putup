// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "options.hpp"
#include "pup/core/result.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>

namespace pup {
struct ProjectLayout;
namespace parser {
class VarDb;
}
namespace graph {
class BuildGraph;
class DepScannerRegistry;
class RulePatternRegistry;
}
}

namespace pup::cli {

/// Options for building the dependency graph
struct BuildContextOptions {
    bool verbose = false;
    bool keep_going = false;
    bool auto_init = false;
    graph::DepScannerRegistry* scanner_registry = nullptr;
    graph::RulePatternRegistry* pattern_registry = nullptr;
};

/// Create scanner registry for implicit dependency tracking
/// Returns nullopt if PUP_IMPLICIT_DEPS=0
[[nodiscard]]
auto make_scanner_registry() -> std::optional<graph::DepScannerRegistry>;

/// Build context using PIMPL to hide heavy dependencies
class BuildContext {
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
    [[nodiscard]]
    auto config_vars() const -> parser::VarDb const&;
    [[nodiscard]]
    auto vars() const -> parser::VarDb const&;
    [[nodiscard]]
    auto parsed_dirs() const -> std::set<std::filesystem::path> const&;

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

/// Context for clean commands
struct CleanContext {
    std::filesystem::path root;
    std::filesystem::path build_dir;
    bool is_in_tree;
};

/// Resolve clean context from options
[[nodiscard]]
auto resolve_clean_context(
    Options const& opts
) -> std::optional<CleanContext>;

} // namespace pup::cli
