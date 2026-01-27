// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/context.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/platform.hpp"
#include "pup/graph/builder.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/gcc.hpp"
#include "pup/index/reader.hpp"
#include "pup/parser/config.hpp"
#include "pup/parser/ignore.hpp"
#include "pup/parser/parser.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

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

    // If cwd is under or equals output_root (but not source_root for in-tree builds),
    // build all. The user is in the build directory, not a source subdirectory.
    // Source files are under source_root, not output_root, so scoping to output_root
    // would incorrectly skip all source file change detection.
    auto output_root = std::filesystem::canonical(layout.output_root);
    if (source_root != output_root && pup::is_path_under(cwd, output_root)) {
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

// Returns empty path for root-equivalent paths ("" or "."), otherwise unchanged
auto normalize_to_empty(std::filesystem::path const& p) -> std::filesystem::path
{
    return (p.empty() || p == ".") ? std::filesystem::path {} : p;
}

// Returns "." for root-equivalent paths ("" or "."), otherwise unchanged
auto normalize_to_dot(std::filesystem::path const& p) -> std::filesystem::path
{
    return (p.empty() || p == ".") ? std::filesystem::path { "." } : p;
}

// Joins base/rel, but if rel is root-equivalent returns just base
auto join_path(std::filesystem::path const& base, std::filesystem::path const& rel)
    -> std::filesystem::path
{
    return (rel.empty() || rel == ".") ? base : base / rel;
}

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
        dirs.insert(normalize_to_dot(dir_rel));
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
    auto normalized = normalize_to_empty(rel_dir);

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
            fprintf(stderr, "Warning: Failed to parse %s: %s\n", config_path.string().c_str(), config_result.error().message.c_str());
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

auto make_circular_dep_error(std::filesystem::path const& dir) -> pup::Error
{
    return pup::Error {
        pup::ErrorCode::CyclicDependency,
        std::format("Circular Tupfile dependency: {}", dir.string())
    };
}

auto make_read_error(std::filesystem::path const& path) -> pup::Error
{
    return pup::Error {
        pup::ErrorCode::IoError,
        std::format("Failed to read {}", path.string())
    };
}

struct ParseContext {
    TupfileParseState& state;
    pup::graph::GraphBuilder& builder;
    pup::graph::BuildGraph& graph;
    std::filesystem::path const& source_root;
    std::filesystem::path const& config_root;
    std::filesystem::path const& output_root;
    pup::parser::VarDb const& base_vars;
    bool verbose;
    bool root_config_only;
    VarAssignedCallback on_var_assigned;
};

auto parse_directory(std::filesystem::path const& rel_dir, ParseContext& ctx) -> pup::Result<void>
{
    auto vars = pup::parser::VarDb { ctx.base_vars };
    auto normalized_dir = normalize_to_dot(rel_dir);

    if (ctx.state.parsed.contains(normalized_dir)) {
        return {};
    }

    if (ctx.state.parsing.contains(normalized_dir)) {
        return pup::unexpected<pup::Error>(make_circular_dep_error(normalized_dir));
    }

    ctx.state.parsing.insert(normalized_dir);

    // Tupfiles are found in config_root (may differ from source_root in 3-tree builds)
    auto tupfile_path = join_path(ctx.config_root, normalize_to_empty(rel_dir)) / "Tupfile";

    if (ctx.verbose) {
        printf("Parsing: %s\n", tupfile_path.string().c_str());
    }

    auto source = read_file(tupfile_path);
    if (!source) {
        ctx.state.parsing.erase(normalized_dir);
        return pup::unexpected<pup::Error>(make_read_error(tupfile_path));
    }

    auto parse_result = pup::parser::parse_tupfile(*source, tupfile_path.string());
    if (!parse_result.success()) {
        ctx.state.parsing.erase(normalized_dir);
        for (auto const& err : parse_result.errors) {
            fprintf(stderr, "%s:%u:%u: error: %s\n", tupfile_path.string().c_str(), err.location.line, err.location.column, err.message.c_str());
        }
        return pup::make_error<void>(pup::ErrorCode::ParseError, "Parse failed");
    }

    auto tup_cwd = normalized_dir.string();
    auto tup_variantdir = compute_tup_variantdir(normalize_to_empty(rel_dir), ctx.config_root, ctx.output_root);

    // Compute TUP_SRCDIR: relative path from config dir to source dir
    // For traditional builds (config == source): "."
    // For 3-tree builds: e.g., "../../busybox/coreutils" from config/coreutils/
    auto rel_dir_normalized = normalize_to_empty(rel_dir);
    auto tup_srcdir = std::string { "." };
    if (ctx.config_root != ctx.source_root) {
        auto config_dir = join_path(ctx.config_root, rel_dir_normalized);
        auto source_dir = join_path(ctx.source_root, rel_dir_normalized);
        tup_srcdir = std::filesystem::relative(source_dir, config_dir).string();
    }

    // Compute TUP_OUTDIR: relative path from config dir to output dir
    // For in-tree builds (config == output): "."
    // For variant builds: e.g., "../../build/coreutils" from config/coreutils/
    auto tup_outdir = std::string { "." };
    if (ctx.config_root != ctx.output_root) {
        auto config_dir = join_path(ctx.config_root, rel_dir_normalized);
        auto output_dir = join_path(ctx.output_root, rel_dir_normalized);
        tup_outdir = std::filesystem::relative(output_dir, config_dir).string();
    }

    // Get the scoped config for this directory (walks up tree to find nearest tup.config)
    // When root_config_only is set (for configure pass), always use root config
    auto const* scoped_config = find_config_for_dir(
        ctx.root_config_only ? std::filesystem::path {} : rel_dir,
        ctx.output_root,
        ctx.state
    );

    auto request_directory = [&](std::filesystem::path const& dir) -> pup::Result<void> {
        return parse_directory(dir, ctx);
    };

    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .config_vars = scoped_config,
        .tup_cwd = tup_cwd,
        .tup_platform = pup::get_platform(),
        .tup_arch = std::string { pup::ARCH },
        .tup_variantdir = tup_variantdir,
        .tup_variant_outputdir = tup_variantdir,
        .tup_srcdir = tup_srcdir,
        .tup_outdir = tup_outdir,
        .request_directory = request_directory,
        .available_tupfile_dirs = &ctx.state.available,
        .on_var_assigned = ctx.on_var_assigned,
    };

    auto result = pup::Result<void> { ctx.builder.add_tupfile(ctx.graph, parse_result.tupfile, eval_ctx) };

    ctx.state.parsing.erase(normalized_dir);
    ctx.state.parsed.insert(normalized_dir);

    if (result) {
        ++pup::thread_metrics().tupfiles_parsed;
    }

    return result;
}

auto make_layout_options(Options const& opts) -> LayoutOptions
{
    auto layout_opts = LayoutOptions {};
    if (!opts.source_dir.empty()) {
        layout_opts.source_dir = std::filesystem::path { opts.source_dir };
    }
    if (!opts.config_dir.empty()) {
        layout_opts.config_dir = std::filesystem::path { opts.config_dir };
    }
    if (!opts.build_dirs.empty()) {
        layout_opts.build_dir = std::filesystem::path { opts.build_dirs[0] };
    }
    return layout_opts;
}

auto try_auto_init(ProjectLayout const& layout) -> void
{
    auto pup_dir = layout.pup_dir();
    if (std::filesystem::exists(pup_dir)) {
        return;
    }
    if (!std::filesystem::exists(layout.source_root / "Tupfile.ini")) {
        return;
    }
    std::filesystem::create_directories(pup_dir);
    printf("Initialized pup in \"%s\"\n", pup_dir.string().c_str());
}

auto load_ignore_list(ProjectLayout const& layout, bool verbose) -> pup::parser::IgnoreList
{
    auto ignore = pup::parser::IgnoreList::with_defaults();
    for (auto const& root : { layout.config_root, layout.source_root }) {
        auto ignore_path = root / ".pupignore";
        if (!std::filesystem::exists(ignore_path)) {
            continue;
        }
        auto ignore_result = pup::parser::IgnoreList::load(ignore_path);
        if (!ignore_result) {
            continue;
        }
        ignore = std::move(*ignore_result);
        if (verbose) {
            printf("Loaded %zu ignore patterns from %s\n", ignore.size(), ignore_path.string().c_str());
        }
        break;
    }
    return ignore;
}

struct IndexLoadResult {
    std::optional<pup::index::Index> index;
    std::unordered_map<std::string, std::string> cached_env_vars;
};

auto load_old_index(std::filesystem::path const& output_root, bool verbose) -> IndexLoadResult
{
    auto result = IndexLoadResult {};
    auto index_path = output_root / ".pup" / "index";

    if (!std::filesystem::exists(index_path)) {
        return result;
    }

    auto index_load_start = std::chrono::steady_clock::now();
    auto index_result = pup::index::read_index(index_path);
    if (!index_result) {
        return result;
    }

    result.index = std::move(*index_result);
    result.index->build_children_index();

    auto index_load_end = std::chrono::steady_clock::now();
    pup::thread_metrics().index_load_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        index_load_end - index_load_start
    );

    constexpr auto ENV_VAR_DIR_PREFIX = std::string_view { "$/" };
    for (auto const& file : result.index->files()) {
        if (file.type != pup::NodeType::Variable) {
            continue;
        }
        if (!file.path.starts_with(ENV_VAR_DIR_PREFIX)) {
            continue;
        }
        auto key_value = std::string_view { file.path }.substr(ENV_VAR_DIR_PREFIX.size());
        auto eq_pos = key_value.find('=');
        if (eq_pos != std::string::npos) {
            result.cached_env_vars[std::string { key_value.substr(0, eq_pos) }] = std::string { key_value.substr(eq_pos + 1) };
        }
    }

    if (verbose && !result.cached_env_vars.empty()) {
        printf("Loaded %zu cached env vars from index\n", result.cached_env_vars.size());
    }

    return result;
}

auto sort_dirs_by_depth(std::set<std::filesystem::path> const& available) -> std::vector<std::filesystem::path>
{
    auto root_rel = std::filesystem::path { "." };
    auto dirs = std::vector<std::filesystem::path> { available.begin(), available.end() };
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
    return dirs;
}

} // namespace

struct BuildContext::Impl {
    ProjectLayout layout;
    parser::VarDb config_vars;
    parser::VarDb vars;
    graph::BuildGraph graph;
    TupfileParseState state;
    std::optional<index::Index> old_index;
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

auto BuildContext::old_index() const -> index::Index const*
{
    return impl_->old_index ? &*impl_->old_index : nullptr;
}

auto build_context(
    Options const& opts,
    BuildContextOptions const& ctx_opts
) -> Result<BuildContext>
{
    // 1. Discover layout
    auto layout_result = Result<ProjectLayout> { discover_layout(make_layout_options(opts)) };
    if (!layout_result) {
        return unexpected<Error>(layout_result.error());
    }

    auto ctx = BuildContext {};
    ctx.impl_->layout = std::move(*layout_result);

    // Set build root name for variant builds (before parsing)
    if (ctx.impl_->layout.source_root != ctx.impl_->layout.output_root) {
        auto build_root_name = std::filesystem::relative(
                                   ctx.impl_->layout.output_root,
                                   ctx.impl_->layout.source_root
        )
                                   .string();
        ctx.impl_->graph.set_build_root_name(std::move(build_root_name));
    }

    // 2. Auto-init if needed
    if (ctx_opts.auto_init) {
        try_auto_init(ctx.impl_->layout);
    }

    // 3. Discover Tupfiles
    auto ignore = load_ignore_list(ctx.impl_->layout, ctx_opts.verbose);
    ctx.impl_->state.available = discover_tupfile_dirs(ctx.impl_->layout.config_root, ignore);

    if (ctx.impl_->state.available.empty()) {
        return make_error<BuildContext>(ErrorCode::IoError, "No Tupfiles found in project");
    }

    if (ctx_opts.verbose) {
        printf("Found %zu directories with Tupfiles\n", ctx.impl_->state.available.size());
    }

    // 4. Load config
    auto config_path = ctx.impl_->layout.output_root / "tup.config";
    if (std::filesystem::exists(config_path)) {
        auto config_result = Result<parser::VarDb> { parser::parse_config(config_path) };
        if (config_result) {
            ctx.impl_->config_vars = std::move(*config_result);
            if (ctx_opts.verbose) {
                printf("Loaded %zu config variables from %s\n", ctx.impl_->config_vars.names().size(), config_path.string().c_str());
            }
        }
    }

    // 5. Load index
    auto [old_index, cached_env_vars] = load_old_index(ctx.impl_->layout.output_root, ctx_opts.verbose);
    ctx.impl_->old_index = std::move(old_index);

    // 6. Parse Tupfiles
    auto builder_opts = graph::BuilderOptions {
        .source_root = ctx.impl_->layout.source_root,
        .config_root = ctx.impl_->layout.config_root,
        .output_root = ctx.impl_->layout.output_root,
        .config_path = config_path,
        .expand_globs = true,
        .scanner_registry = ctx_opts.scanner_registry,
        .pattern_registry = ctx_opts.pattern_registry,
        .cached_env_vars = std::move(cached_env_vars),
    };
    auto builder = graph::GraphBuilder { builder_opts };

    auto parse_ctx = ParseContext {
        .state = ctx.impl_->state,
        .builder = builder,
        .graph = ctx.impl_->graph,
        .source_root = ctx.impl_->layout.source_root,
        .config_root = ctx.impl_->layout.config_root,
        .output_root = ctx.impl_->layout.output_root,
        .base_vars = ctx.impl_->vars,
        .verbose = ctx_opts.verbose,
        .root_config_only = ctx_opts.root_config_only,
        .on_var_assigned = ctx_opts.on_var_assigned,
    };

    for (auto const& dir : sort_dirs_by_depth(ctx.impl_->state.available)) {
        if (ctx.impl_->state.parsed.contains(dir)) {
            continue;
        }
        auto result = Result<void> { parse_directory(dir, parse_ctx) };
        if (!result && !ctx_opts.keep_going) {
            return unexpected<Error>(result.error());
        }
    }

    // Resolve deferred order-only edges (side effect: modifies graph; return value is count, unused)
    (void)builder.resolve_deferred_order_only_edges(ctx.impl_->graph);

    for (auto const& warning : builder.warnings()) {
        fprintf(stderr, "warning: %s\n", warning.c_str());
    }

    if (ctx_opts.verbose) {
        printf("Parsed %zu Tupfiles\n", ctx.impl_->state.parsed.size());
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
