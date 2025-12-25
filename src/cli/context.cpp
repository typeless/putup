// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/context.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/platform.hpp"
#include "pup/graph/builder.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/gcc.hpp"
#include "pup/parser/config.hpp"
#include "pup/parser/ignore.hpp"
#include "pup/parser/parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include <fmt/core.h>

namespace pup::cli {

auto make_scanner_registry() -> std::optional<graph::DepScannerRegistry>
{
    if (auto const* env = std::getenv("PUP_IMPLICIT_DEPS"); env && std::string_view { env } == "0") {
        return std::nullopt;
    }
    auto registry = graph::DepScannerRegistry {};
    registry.register_scanner(graph::scanners::make_gcc_scanner());
    return registry;
}

auto compute_build_scopes(
    Options const& opts,
    ProjectLayout const& layout
) -> std::vector<std::string>
{
    // -A/--all flag forces full project build
    if (opts.all) {
        return {};
    }

    // Explicit targets as scopes
    if (!opts.targets.empty()) {
        return opts.targets;
    }

    // Compute scope from current working directory
    auto cwd = std::filesystem::current_path();
    auto source_root = std::filesystem::canonical(layout.source_root);

    // If cwd is source_root, build all
    if (cwd == source_root) {
        return {};
    }

    // Get relative path if cwd is under source_root
    auto rel = pup::relative_to_root(cwd, source_root);
    if (rel.empty()) {
        return {};
    }

    return std::vector<std::string> { rel };
}

namespace {

/// State for tracking Tupfile parsing across multiple directories
struct TupfileParseState {
    std::set<std::filesystem::path> available;
    std::set<std::filesystem::path> parsed;
    std::set<std::filesystem::path> parsing;
    std::map<std::filesystem::path, parser::VarDb> scoped_configs; // Cache of per-dir configs
};

auto compute_tup_variantdir(
    std::filesystem::path const& source_dir,
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root
) -> std::string
{
    if (!output_root.empty() && source_root != output_root) {
        auto output_dir = output_root / source_dir;
        auto src_dir = source_root / source_dir;
        auto rel = std::filesystem::relative(output_dir, src_dir);
        return rel.string();
    }

    return ".";
}

auto find_build_subdir(
    std::filesystem::path const& root
) -> std::optional<std::filesystem::path>
{
    for (auto const& name : { "build", "out", "variant" }) {
        auto dir = std::filesystem::path { root / name };
        if (std::filesystem::exists(dir / "tup.config")
            || std::filesystem::is_directory(dir / ".pup")) {
            return dir;
        }
    }

    if (std::filesystem::is_directory(root)) {
        for (auto const& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory()) {
                if (std::filesystem::exists(entry.path() / "tup.config")
                    || std::filesystem::is_directory(entry.path() / ".pup")) {
                    return entry.path();
                }
            }
        }
    }

    return std::nullopt;
}

auto read_file(std::filesystem::path const& path) -> std::optional<std::string>
{
    auto file = std::ifstream { path };
    if (!file) {
        return std::nullopt;
    }

    auto ss = std::stringstream {};
    ss << file.rdbuf();
    return ss.str();
}

auto discover_tupfile_dirs(
    std::filesystem::path const& root,
    pup::parser::IgnoreList const& ignore = {}
) -> std::set<std::filesystem::path>
{
    auto dirs = std::set<std::filesystem::path> {};
    auto ec = std::error_code {};
    auto options = std::filesystem::directory_options::skip_permission_denied;

    for (auto it = std::filesystem::recursive_directory_iterator(root, options, ec);
         it != std::filesystem::recursive_directory_iterator();
         ++it) {
        if (ec) {
            break;
        }

        auto const& entry = *it;
        auto rel = std::filesystem::relative(entry.path(), root);

        if (entry.is_directory() && ignore.is_ignored(rel)) {
            it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().filename() != "Tupfile") {
            continue;
        }

        auto dir = std::filesystem::path { entry.path().parent_path() };

        // Skip variant directories (have tup.config but are not the source root)
        // The source root may have both Tupfile and tup.config
        if (dir != root && std::filesystem::exists(dir / "tup.config")) {
            continue;
        }

        auto dir_rel = std::filesystem::relative(dir, root);
        dirs.insert(dir_rel.empty() || dir_rel == "." ? std::filesystem::path { "." } : dir_rel);
    }

    return dirs;
}

/// Find the tup.config for a directory by walking up the tree
/// Returns pointer to the cached VarDb for that directory
auto find_config_for_dir(
    std::filesystem::path const& rel_dir,
    std::filesystem::path const& output_root,
    TupfileParseState& state
) -> parser::VarDb const*
{
    auto normalized = std::filesystem::path {
        rel_dir.empty() || rel_dir == "." ? std::filesystem::path {} : rel_dir
    };

    // Check cache first
    if (auto it = state.scoped_configs.find(normalized); it != state.scoped_configs.end()) {
        return &it->second;
    }

    // Walk up from output_root/dir/ looking for tup.config
    auto search_path = output_root / normalized;
    while (true) {
        auto config_path = search_path / "tup.config";
        if (std::filesystem::exists(config_path)) {
            // Found a config - load and cache it
            auto config_result = parser::parse_config(config_path);
            if (config_result) {
                auto [it, _] = state.scoped_configs.emplace(normalized, std::move(*config_result));
                return &it->second;
            }
            // Parse failed - warn user and return empty config (blocks inheritance)
            fmt::print(stderr, "Warning: Failed to parse {}: {}\n", config_path.string(), config_result.error().message);
            auto [it, _] = state.scoped_configs.emplace(normalized, parser::VarDb {});
            return &it->second;
        }

        // Check if we've reached the output_root
        if (search_path == output_root || !search_path.has_parent_path()
            || search_path.parent_path() == search_path) {
            break;
        }

        search_path = search_path.parent_path();
    }

    // No config found - cache empty config
    auto [it, _] = state.scoped_configs.emplace(normalized, parser::VarDb {});
    return &it->second;
}

auto parse_directory(
    std::filesystem::path const& rel_dir,
    TupfileParseState& state,
    pup::graph::GraphBuilder& builder,
    pup::graph::BuildGraph& graph,
    std::filesystem::path const& root,
    std::filesystem::path const& output_root,
    pup::parser::VarDb const& base_vars,
    bool verbose,
    bool root_config_only = false
) -> pup::Result<void>
{
    auto vars = pup::parser::VarDb { base_vars };
    auto normalized_dir = std::filesystem::path {
        rel_dir.empty() || rel_dir == "." ? std::filesystem::path { "." } : rel_dir
    };

    if (state.parsed.contains(normalized_dir)) {
        return {};
    }

    if (state.parsing.contains(normalized_dir)) {
        return pup::make_error<void>(
            pup::ErrorCode::CyclicDependency,
            fmt::format("Circular Tupfile dependency: {}", normalized_dir.string())
        );
    }

    state.parsing.insert(normalized_dir);

    auto tupfile_path = std::filesystem::path {
        normalized_dir == "." ? root / "Tupfile" : root / rel_dir / "Tupfile"
    };

    if (verbose) {
        fmt::print("Parsing: {}\n", tupfile_path.string());
    }

    auto source = read_file(tupfile_path);
    if (!source) {
        state.parsing.erase(normalized_dir);
        return pup::make_error<void>(
            pup::ErrorCode::IoError,
            fmt::format("Failed to read {}", tupfile_path.string())
        );
    }

    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto parse_result = pup::Result<pup::parser::Tupfile> { parser.parse() };
    if (!parse_result) {
        state.parsing.erase(normalized_dir);
        for (auto const& err : parser.errors()) {
            fmt::print(stderr, "{}:{}:{}: error: {}\n",
                tupfile_path.string(),
                err.location.line,
                err.location.column,
                err.message);
        }
        return pup::unexpected<pup::Error>(parse_result.error());
    }

    auto tup_cwd = std::string { normalized_dir == "." ? "." : rel_dir.string() };
    auto tup_variantdir = compute_tup_variantdir(
        normalized_dir == "." ? std::filesystem::path {} : rel_dir,
        root,
        output_root
    );

    // Get the scoped config for this directory (walks up tree to find nearest tup.config)
    // When root_config_only is set (for configure pass), always use root config
    auto const* scoped_config = find_config_for_dir(
        root_config_only ? std::filesystem::path {} : rel_dir,
        output_root,
        state
    );

    auto request_directory = [&](std::filesystem::path const& dir) -> pup::Result<void> {
        return parse_directory(dir, state, builder, graph, root, output_root, base_vars, verbose, root_config_only);
    };

    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .config_vars = scoped_config,
        .tup_cwd = tup_cwd,
        .tup_platform = pup::get_platform(),
        .tup_arch = std::string { pup::ARCH },
        .tup_variantdir = tup_variantdir,
        .tup_variant_outputdir = tup_variantdir,
        .request_directory = request_directory,
        .available_tupfile_dirs = &state.available,
    };

    auto result = pup::Result<void> { builder.add_tupfile(graph, *parse_result, eval_ctx) };

    state.parsing.erase(normalized_dir);
    state.parsed.insert(normalized_dir);

    if (result) {
        ++pup::thread_metrics().tupfiles_parsed;
    }

    return result;
}

} // namespace

struct BuildContext::Impl {
    ProjectLayout layout;
    parser::VarDb config_vars;
    parser::VarDb vars;
    graph::BuildGraph graph;
    TupfileParseState state;
};

BuildContext::BuildContext()
    : impl_(std::make_unique<Impl>())
{
}

BuildContext::~BuildContext() = default;

BuildContext::BuildContext(BuildContext&&) noexcept = default;

auto BuildContext::operator=(BuildContext&&) noexcept -> BuildContext& = default;

auto BuildContext::layout() const -> ProjectLayout const&
{
    return impl_->layout;
}

auto BuildContext::graph() const -> graph::BuildGraph const&
{
    return impl_->graph;
}

auto BuildContext::graph() -> graph::BuildGraph&
{
    return impl_->graph;
}

auto BuildContext::config_vars() const -> parser::VarDb const&
{
    return impl_->config_vars;
}

auto BuildContext::vars() const -> parser::VarDb const&
{
    return impl_->vars;
}

auto BuildContext::parsed_dirs() const -> std::set<std::filesystem::path> const&
{
    return impl_->state.parsed;
}

auto build_context(
    Options const& opts,
    BuildContextOptions const& ctx_opts
) -> Result<BuildContext>
{
    auto layout_opts = LayoutOptions {};
    if (!opts.source_dir.empty()) {
        layout_opts.source_dir = std::filesystem::path { opts.source_dir };
    }
    if (!opts.build_dirs.empty()) {
        layout_opts.build_dir = std::filesystem::path { opts.build_dirs[0] };
    }

    auto layout_result = Result<ProjectLayout> { discover_layout(layout_opts) };
    if (!layout_result) {
        return unexpected<Error>(layout_result.error());
    }

    auto ctx = BuildContext {};
    ctx.impl_->layout = std::move(*layout_result);

    if (ctx_opts.auto_init) {
        auto pup_dir = ctx.impl_->layout.pup_dir();
        if (!std::filesystem::exists(pup_dir)) {
            if (std::filesystem::exists(ctx.impl_->layout.source_root / "Tupfile.ini")) {
                std::filesystem::create_directories(pup_dir);
                fmt::print("Initialized pup in \"{}\"\n", pup_dir.string());
            }
        }
    }

    auto ignore = parser::IgnoreList::with_defaults();
    auto ignore_path = ctx.impl_->layout.source_root / ".pupignore";
    if (std::filesystem::exists(ignore_path)) {
        auto ignore_result = parser::IgnoreList::load(ignore_path);
        if (ignore_result) {
            ignore = std::move(*ignore_result);
            if (ctx_opts.verbose) {
                fmt::print("Loaded {} ignore patterns from {}\n", ignore.size(), ignore_path.string());
            }
        }
    }

    ctx.impl_->state.available = discover_tupfile_dirs(ctx.impl_->layout.source_root, ignore);

    if (ctx.impl_->state.available.empty()) {
        return make_error<BuildContext>(
            ErrorCode::IoError, "No Tupfiles found in project"
        );
    }

    if (ctx_opts.verbose) {
        fmt::print("Found {} directories with Tupfiles\n", ctx.impl_->state.available.size());
    }

    // tup.config lives in output_root (variant or -B directory)
    auto config_path = ctx.impl_->layout.output_root / "tup.config";
    if (std::filesystem::exists(config_path)) {
        auto config_result = Result<parser::VarDb> { parser::parse_config(config_path) };
        if (config_result) {
            ctx.impl_->config_vars = std::move(*config_result);
            if (ctx_opts.verbose) {
                fmt::print("Loaded {} config variables from {}\n", ctx.impl_->config_vars.names().size(), config_path.string());
            }
        }
    }

    auto builder_opts = graph::BuilderOptions {
        .source_root = ctx.impl_->layout.source_root,
        .output_root = ctx.impl_->layout.output_root,
        .config_path = config_path,
        .expand_globs = true,
        .scanner_registry = ctx_opts.scanner_registry,
        .pattern_registry = ctx_opts.pattern_registry,
    };

    auto builder = graph::GraphBuilder { builder_opts };

    // Post-order traversal: parse children before parents so groups are populated
    // before parent directories reference them. Sort by path depth (deeper first).
    // Root directory "." is always parsed last.
    auto root_rel = std::filesystem::path { "." };
    auto dirs = std::vector<std::filesystem::path> {
        ctx.impl_->state.available.begin(), ctx.impl_->state.available.end()
    };
    std::ranges::sort(dirs, [&root_rel](auto const& a, auto const& b) {
        auto is_root_a = (a == root_rel);
        auto is_root_b = (b == root_rel);
        if (is_root_a != is_root_b) {
            return is_root_b;
        }
        auto depth_a = std::distance(a.begin(), a.end());
        auto depth_b = std::distance(b.begin(), b.end());
        if (depth_a != depth_b) {
            return depth_a > depth_b;
        }
        return a < b;
    });

    for (auto const& dir : dirs) {
        if (ctx.impl_->state.parsed.contains(dir)) {
            continue;
        }
        auto result = Result<void> {
            parse_directory(dir, ctx.impl_->state, builder, ctx.impl_->graph, ctx.impl_->layout.source_root, ctx.impl_->layout.output_root, ctx.impl_->vars, ctx_opts.verbose, ctx_opts.root_config_only)
        };
        if (!result && !ctx_opts.keep_going) {
            return unexpected<Error>(result.error());
        }
    }

    // Resolve order-only edges that couldn't be created during parsing due to
    // circular Tupfile dependencies (e.g., A references B's group, B's parsing
    // triggers A's parsing before B's group is registered)
    (void)builder.resolve_deferred_order_only_edges(ctx.impl_->graph);

    // Surface any warnings from graph building (e.g., undefined order-only groups)
    for (auto const& warning : builder.warnings()) {
        fmt::print(stderr, "warning: {}\n", warning);
    }

    if (ctx_opts.verbose) {
        fmt::print("Parsed {} Tupfiles\n", ctx.impl_->state.parsed.size());
    }

    return ctx;
}

auto resolve_clean_context(Options const& opts) -> std::optional<CleanContext>
{
    auto cwd = std::filesystem::current_path();
    auto root = find_project_root(cwd);
    if (!root) {
        return std::nullopt;
    }

    auto build_dir = std::filesystem::path {};
    auto is_in_tree = false;

    if (!opts.build_dirs.empty()) {
        build_dir = std::filesystem::path { opts.build_dirs[0] };
        if (build_dir.is_relative()) {
            build_dir = *root / build_dir;
        }
        is_in_tree = (build_dir == *root);
    } else if (std::filesystem::exists(cwd / ".pup") && cwd != *root) {
        // cwd contains .pup and is not source root - we're inside a build directory
        build_dir = cwd;
        is_in_tree = false;
    } else if (auto detected = find_build_subdir(*root)) {
        // Prefer build subdirectory with .pup/index over source root
        build_dir = *detected;
        is_in_tree = false;
    } else if (std::filesystem::exists(*root / "tup.config")
               || std::filesystem::exists(*root / ".pup")) {
        // Fall back to source root for in-tree builds
        build_dir = *root;
        is_in_tree = true;
    } else {
        return std::nullopt;
    }

    return CleanContext { *root, build_dir, is_in_tree };
}

} // namespace pup::cli
