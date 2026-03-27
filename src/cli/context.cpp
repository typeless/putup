// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/context.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/builder.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/gcc.hpp"
#include "pup/index/reader.hpp"
#include "pup/parser/config.hpp"
#include "pup/parser/ignore.hpp"
#include "pup/parser/parser.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

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
) -> Vec<StringId>
{
    auto& pool = global_pool();

    // -A/--all flag forces full project build
    if (opts.all) {
        return {};
    }

    // Explicit targets as scopes
    if (!opts.targets.empty()) {
        return opts.targets;
    }

    // Compute scope from current working directory
    auto cwd = *pup::platform::current_directory();
    auto source_root_sv = pool.get(layout.source_root);

    // If cwd is source_root, build all
    if (cwd == source_root_sv) {
        return {};
    }

    // If cwd is under or equals output_root (but not source_root for in-tree builds),
    // build all. The user is in the build directory, not a source subdirectory.
    // Source files are under source_root, not output_root, so scoping to output_root
    // would incorrectly skip all source file change detection.
    auto output_root_sv = pool.get(layout.output_root);
    if (layout.source_root != layout.output_root && pup::is_path_under(cwd, output_root_sv)) {
        return {};
    }

    // Get relative path if cwd is under source_root
    auto rel = pup::relative_to_root(cwd, source_root_sv);
    if (is_empty(rel)) {
        return {};
    }

    return Vec<StringId> { rel };
}

namespace {

// Returns empty path for root-equivalent paths ("" or "."), otherwise unchanged
auto normalize_to_empty(String const& p) -> String
{
    return (p.empty() || p == ".") ? String {} : p;
}

// Returns "." for root-equivalent paths ("" or "."), otherwise unchanged
auto normalize_to_dot(String const& p) -> String
{
    return (p.empty() || p == ".") ? String { "." } : p;
}

// Joins base/rel, but if rel is root-equivalent returns just base
auto join_path(std::string_view base, std::string_view rel)
    -> String
{
    return (rel.empty() || rel == ".") ? String { base } : String { pup::global_pool().get(pup::path::join(base, rel)) };
}

auto sorted_contains(Vec<StringId> const& v, std::string_view key) -> bool
{
    auto& pool = global_pool();
    auto id = pool.find(key);
    if (is_empty(id)) {
        return false;
    }
    for (auto item : v) {
        if (item == id) {
            return true;
        }
    }
    return false;
}

auto sorted_insert(Vec<StringId>& v, std::string_view key) -> void
{
    auto id = global_pool().intern(key);
    for (auto item : v) {
        if (item == id) {
            return;
        }
    }
    v.push_back(id);
}

auto sorted_erase(Vec<StringId>& v, std::string_view key) -> void
{
    auto& pool = global_pool();
    auto id = pool.find(key);
    for (auto i = std::size_t { 0 }; i < v.size(); ++i) {
        if (v[i] == id) {
            v.erase(v.data() + i);
            return;
        }
    }
}

/// State for tracking Tupfile parsing across multiple directories
struct TupfileParseState {
    Vec<StringId> available;
    Vec<StringId> parsed;
    Vec<StringId> parsing;
    // Append-only paged vectors: push_back preserves references to existing
    // elements, which is critical because recursive Tupfile parsing holds
    // VarDb pointers across calls that may insert new entries.
    PagedVec<std::pair<String, parser::VarDb>> parsed_configs;
    PagedVec<std::pair<String, parser::VarDb>> scoped_configs;
    Vec<std::pair<pup::StringId, pup::StringId>> const* config_defines = nullptr; // CLI overrides
};

auto compute_tup_variantdir(
    std::string_view source_dir,
    std::string_view source_root,
    std::string_view output_root
) -> String
{
    auto& pool = pup::global_pool();
    if (!output_root.empty() && source_root != output_root) {
        auto output_dir = pool.get(pup::path::join(output_root, source_dir));
        auto src_dir = pool.get(pup::path::join(source_root, source_dir));
        auto src_canonical = pup::platform::canonical(src_dir);
        auto out_canonical = pup::platform::canonical(output_dir);
        if (src_canonical && out_canonical) {
            return String { pool.get(pup::path::relative(*out_canonical, *src_canonical)) };
        }
        return ".";
    }

    return ".";
}

auto find_build_subdir(
    std::string_view root
) -> std::optional<String>
{
    auto& pool = global_pool();
    for (auto const& name : { "build", "out", "variant" }) {
        auto dir = pool.get(pup::path::join(root, name));
        if (pup::platform::exists(pool.get(pup::path::join(dir, "tup.config")))
            || pup::platform::is_directory(pool.get(pup::path::join(dir, ".pup")))) {
            return String { dir };
        }
    }

    if (pup::platform::is_directory(root)) {
        auto entries = pup::platform::read_directory(root);
        if (entries) {
            for (auto const& entry : *entries) {
                if (!entry.is_dir) {
                    continue;
                }
                auto entry_path = pool.get(pup::path::join(root, pool.get(entry.name)));
                if (pup::platform::exists(pool.get(pup::path::join(entry_path, "tup.config")))
                    || pup::platform::is_directory(pool.get(pup::path::join(entry_path, ".pup")))) {
                    return String { entry_path };
                }
            }
        }
    }

    return std::nullopt;
}

auto read_file(std::string_view path) -> std::optional<String>
{
    auto result = pup::platform::read_file(path);
    if (!result) {
        return std::nullopt;
    }
    return std::move(*result);
}

auto discover_tupfile_dirs(
    std::string_view root,
    pup::parser::IgnoreList const& ignore = {}
) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto dirs = Vec<StringId> {};

    if (pup::platform::exists(pool.get(pup::path::join(root, "Tupfile")))) {
        dirs.push_back(pool.intern("."));
    }

    (void)pup::platform::walk_directory(root, [&](pup::platform::DirEntry const& entry, std::string_view rel_path) -> bool {
        if (entry.is_dir && ignore.is_ignored(rel_path)) {
            return false;
        }

        if (!entry.is_dir && pool.get(entry.name) == "Tupfile") {
            auto dir_rel = String { pup::path::parent(rel_path) };
            dirs.push_back(pool.intern(normalize_to_dot(dir_rel)));
        }

        return true;
    });

    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    return dirs;
}

/// Apply CLI config overrides to a VarDb
auto apply_config_overrides(
    parser::VarDb& config,
    Vec<std::pair<pup::StringId, pup::StringId>> const* defines
) -> void
{
    if (!defines) {
        return;
    }
    auto& pool = global_pool();
    for (auto const& [name, value] : *defines) {
        auto name_sv = pool.get(name);
        auto value_sv = pool.get(value);
        config.set(name_sv, value_sv);
        config.set(String { "CONFIG_" } + name_sv, value_sv);
    }
}

/// Parse a tup.config file, returning a cached result on repeat calls.
auto get_or_parse_config(
    String const& path,
    TupfileParseState& state
) -> parser::VarDb const*
{
    for (auto const& entry : state.parsed_configs) {
        if (entry.first == path) {
            return &entry.second;
        }
    }

    auto result = parser::parse_config(path);
    if (!result) {
        fprintf(stderr, "Warning: Failed to parse %s: %s\n", path.c_str(), result.error().msg().data());
        return nullptr;
    }

    state.parsed_configs.emplace_back(path, std::move(*result));
    return &state.parsed_configs.back().second;
}

/// Merge all tup.config files from root down to target directory.
/// Parent configs override child configs on collision (same authority
/// model as Tuprules.tup ?= defaults).
/// Returns pointer to the cached VarDb for that directory.
auto find_config_for_dir(
    String const& rel_dir,
    String const& output_root,
    TupfileParseState& state
) -> parser::VarDb const*
{
    auto normalized = normalize_to_empty(rel_dir);

    // Check cache first
    for (auto const& entry : state.scoped_configs) {
        if (entry.first == normalized) {
            return &entry.second;
        }
    }

    // Collect all tup.config paths from root down to target directory
    auto config_paths = Vec<String> {};

    auto& pool = pup::global_pool();
    auto root_config_sv = pool.get(pup::path::join(output_root, "tup.config"));
    if (pup::platform::exists(root_config_sv)) {
        config_paths.push_back(String { root_config_sv });
    }

    if (!normalized.empty()) {
        auto accumulated = String { output_root };
        auto remaining = std::string_view { normalized };
        while (!remaining.empty()) {
            auto slash = remaining.find('/');
            auto component = (slash == std::string_view::npos) ? remaining : remaining.substr(0, slash);
            remaining = (slash == std::string_view::npos) ? std::string_view {} : remaining.substr(slash + 1);
            if (component.empty()) {
                continue;
            }
            accumulated = String { pool.get(pup::path::join(accumulated, component)) };
            auto config_path_sv = pool.get(pup::path::join(accumulated, "tup.config"));
            if (pup::platform::exists(config_path_sv)) {
                config_paths.push_back(String { config_path_sv });
            }
        }
    }

    if (config_paths.empty()) {
        state.scoped_configs.emplace_back(normalized, parser::VarDb {});
        return &state.scoped_configs.back().second;
    }

    // Merge leaf first (defaults), then each parent on top (overrides).
    // config_paths is root-to-leaf, so reverse iteration gives leaf→root.
    auto merged = parser::VarDb {};
    for (auto it = config_paths.rbegin(); it != config_paths.rend(); ++it) {
        auto const* cfg = get_or_parse_config(*it, state);
        if (cfg) {
            for (auto const& name : cfg->names()) {
                merged.set(name, cfg->get(name));
            }
        }
    }

    apply_config_overrides(merged, state.config_defines);
    state.scoped_configs.emplace_back(normalized, std::move(merged));
    return &state.scoped_configs.back().second;
}

auto make_circular_dep_error(String const& dir) -> pup::Error
{
    return pup::Error {
        pup::ErrorCode::CyclicDependency,
        String { "Circular Tupfile dependency: " } + dir
    };
}

auto make_read_error(String const& path) -> pup::Error
{
    return pup::Error {
        pup::ErrorCode::IoError,
        String { "Failed to read " } + path
    };
}

struct ParseContext {
    TupfileParseState& state;
    pup::graph::GraphBuilder& builder;
    pup::graph::BuildGraph& graph;
    std::string_view source_root;
    std::string_view config_root;
    std::string_view output_root;
    pup::parser::VarDb const& base_vars;
    bool verbose;
    bool root_config_only;
    VarAssignedCallback on_var_assigned;
};

auto parse_directory(String const& rel_dir, ParseContext& ctx) -> pup::Result<void>
{
    auto vars = pup::parser::VarDb { ctx.base_vars };
    auto normalized_dir = normalize_to_dot(rel_dir);

    if (sorted_contains(ctx.state.parsed, normalized_dir)) {
        return {};
    }

    if (sorted_contains(ctx.state.parsing, normalized_dir)) {
        return pup::unexpected<pup::Error>(make_circular_dep_error(normalized_dir));
    }

    sorted_insert(ctx.state.parsing, normalized_dir);

    // Tupfiles are found in config_root (may differ from source_root in 3-tree builds)
    auto tupfile_path = String { pup::global_pool().get(pup::path::join(join_path(ctx.config_root, normalize_to_empty(rel_dir)), "Tupfile")) };

    if (ctx.verbose) {
        printf("Parsing: %s\n", tupfile_path.c_str());
    }

    auto source = read_file(tupfile_path);
    if (!source) {
        sorted_erase(ctx.state.parsing, normalized_dir);
        return pup::unexpected<pup::Error>(make_read_error(tupfile_path));
    }

    auto parse_result = pup::parser::parse_tupfile(*source, tupfile_path);
    if (!parse_result.success()) {
        sorted_erase(ctx.state.parsing, normalized_dir);
        for (auto const& err : parse_result.errors) {
            fprintf(stderr, "%s:%u:%u: error: %s\n", tupfile_path.c_str(), err.location.line, err.location.column, global_pool().get(err.message).data());
        }
        return pup::make_error<void>(pup::ErrorCode::ParseError, "Parse failed");
    }

    auto tup_cwd = normalized_dir;

    // In the "overlay" model, Tupfiles from config_root are treated as if they
    // were in source_root. Commands run from source_root, so all relative paths
    // (TUP_VARIANTDIR, TUP_OUTDIR, etc.) must be computed relative to source_root.
    auto rel_dir_normalized = normalize_to_empty(rel_dir);
    auto tup_variantdir = compute_tup_variantdir(rel_dir_normalized, ctx.source_root, ctx.output_root);

    // TUP_SRCDIR: relative path to source files from where commands run.
    // In overlay model, commands run from source_root, so TUP_SRCDIR is always "."
    auto tup_srcdir = String { "." };

    // TUP_OUTDIR: relative path from source dir (where commands run) to output dir.
    // For in-tree builds (source == output): "."
    // For variant builds: e.g., "../../build/coreutils" from source/coreutils/
    auto tup_outdir = String { "." };
    if (ctx.source_root != ctx.output_root) {
        auto source_dir = pup::platform::canonical(join_path(ctx.source_root, rel_dir_normalized));
        auto output_dir = pup::platform::canonical(join_path(ctx.output_root, rel_dir_normalized));
        if (source_dir && output_dir) {
            tup_outdir = String { pup::global_pool().get(pup::path::relative(*output_dir, *source_dir)) };
        }
    }

    // Get the scoped config for this directory (walks up tree to find nearest tup.config)
    // When root_config_only is set (for configure pass), always use root config
    auto const* scoped_config = find_config_for_dir(
        ctx.root_config_only ? String {} : rel_dir,
        ctx.output_root,
        ctx.state
    );

    auto request_directory = [&](std::string_view dir) -> pup::Result<void> {
        return parse_directory(String { dir }, ctx);
    };

    auto& pool = global_pool();
    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .config_vars = scoped_config,
        .tup_cwd = pool.intern(tup_cwd),
        .tup_platform = pool.intern(pup::get_platform()),
        .tup_arch = pool.intern(pup::ARCH),
        .tup_variantdir = pool.intern(tup_variantdir),
        .tup_variant_outputdir = pool.intern(tup_variantdir),
        .tup_srcdir = pool.intern(tup_srcdir),
        .tup_outdir = pool.intern(tup_outdir),
        .request_directory = request_directory,
        .available_tupfile_dirs = &ctx.state.available,
        .on_var_assigned = ctx.on_var_assigned,
    };

    auto result = pup::Result<void> { ctx.builder.add_tupfile(ctx.graph, parse_result.tupfile, eval_ctx) };

    sorted_erase(ctx.state.parsing, normalized_dir);
    sorted_insert(ctx.state.parsed, normalized_dir);

    if (result) {
        ++pup::thread_metrics().tupfiles_parsed;
    }

    return result;
}

auto try_auto_init(ProjectLayout const& layout) -> void
{
    auto& pool = global_pool();
    auto pup_dir_sv = pool.get(layout.pup_dir());
    if (pup::platform::exists(pup_dir_sv)) {
        return;
    }
    if (!pup::platform::exists(pool.get(pup::path::join(pool.get(layout.source_root), "Tupfile.ini")))) {
        return;
    }
    (void)pup::platform::create_directories(pup_dir_sv);
    printf("Initialized pup in \"%.*s\"\n", static_cast<int>(pup_dir_sv.size()), pup_dir_sv.data());
}

struct IndexLoadResult {
    std::optional<pup::index::Index> index;
    Vec<std::pair<StringId, StringId>> cached_env_vars;
};

auto load_old_index(std::string_view output_root, bool verbose) -> IndexLoadResult
{
    auto& pool = pup::global_pool();
    auto result = IndexLoadResult {};
    auto index_path = String { pool.get(pup::path::join(pool.get(pup::path::join(output_root, ".pup")), "index")) };

    if (!pup::platform::exists(index_path)) {
        return result;
    }

    auto index_load_start = std::chrono::steady_clock::now();
    auto index_result = pup::index::read_index(index_path);
    if (!index_result) {
        return result;
    }

    result.index = std::move(*index_result);

    auto index_load_end = std::chrono::steady_clock::now();
    pup::thread_metrics().index_load_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        index_load_end - index_load_start
    );

    constexpr auto ENV_VAR_DIR_PREFIX = std::string_view { "$/" };
    for (auto const& file : result.index->files()) {
        if (file.type != pup::NodeType::Variable) {
            continue;
        }
        auto file_path_sv = pup::global_pool().get(file.path);
        if (!file_path_sv.starts_with(ENV_VAR_DIR_PREFIX)) {
            continue;
        }
        auto key_value = file_path_sv.substr(ENV_VAR_DIR_PREFIX.size());
        auto eq_pos = key_value.find('=');
        if (eq_pos != std::string_view::npos) {
            result.cached_env_vars.emplace_back(pool.intern(key_value.substr(0, eq_pos)), pool.intern(key_value.substr(eq_pos + 1)));
        }
    }

    std::sort(result.cached_env_vars.begin(), result.cached_env_vars.end());

    if (verbose && !result.cached_env_vars.empty()) {
        printf("Loaded %zu cached env vars from index\n", result.cached_env_vars.size());
    }

    return result;
}

auto sort_dirs_by_depth(Vec<StringId> const& available) -> Vec<StringId>
{
    auto constexpr root_rel = std::string_view { "." };
    auto& pool = global_pool();
    auto dirs = Vec<StringId> { available };
    std::ranges::sort(dirs, [&root_rel, &pool](auto a, auto b) {
        auto a_sv = pool.get(a);
        auto b_sv = pool.get(b);
        auto is_root_a = (a_sv == root_rel);
        auto is_root_b = (b_sv == root_rel);
        if (is_root_a != is_root_b) {
            return is_root_b;
        }
        auto depth_a = static_cast<int>(a_sv.size());
        auto depth_b = static_cast<int>(b_sv.size());
        if (depth_a != depth_b) {
            return depth_a > depth_b;
        }
        return a_sv < b_sv;
    });
    return dirs;
}

auto load_ignore_list(ProjectLayout const& layout, bool verbose) -> pup::parser::IgnoreList
{
    auto ignore = pup::parser::IgnoreList::with_defaults();
    auto& ipool = global_pool();
    for (auto root_id : { layout.config_root, layout.source_root }) {
        auto ignore_path = String { ipool.get(pup::path::join(ipool.get(root_id), ".pupignore")) };
        if (!pup::platform::exists(ignore_path)) {
            continue;
        }
        auto ignore_result = pup::parser::IgnoreList::load(ignore_path);
        if (!ignore_result) {
            continue;
        }
        ignore = std::move(*ignore_result);
        if (verbose) {
            printf("Loaded %zu ignore patterns from %s\n", ignore.size(), ignore_path.c_str());
        }
        break;
    }
    return ignore;
}

} // namespace

auto make_layout_options(Options const& opts) -> LayoutOptions
{
    auto layout_opts = LayoutOptions {};
    if (!is_empty(opts.source_dir)) {
        layout_opts.source_dir = opts.source_dir;
    }
    if (!is_empty(opts.config_dir)) {
        layout_opts.config_dir = opts.config_dir;
    }
    if (!opts.build_dirs.empty()) {
        layout_opts.build_dir = opts.build_dirs[0];
    }
    return layout_opts;
}

struct BuildContext::Impl {
    graph::BuildGraph graph;
    ProjectLayout layout;
    parser::VarDb config_vars;
    parser::VarDb vars;
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

auto BuildContext::parsed_dirs() const -> Vec<StringId> const&
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

    auto& pool = global_pool();

    // Early tup.config check (before expensive parsing)
    if (ctx_opts.require_config) {
        auto config_path_sv = pool.get(pup::path::join(pool.get(ctx.impl_->layout.output_root), "tup.config"));
        if (!pup::platform::exists(config_path_sv)) {
            return make_error<BuildContext>(
                ErrorCode::NotFound,
                "No tup.config found. Run 'pup configure' first."
            );
        }
    }

    // Set build root name for variant builds (before parsing)
    if (ctx.impl_->layout.source_root != ctx.impl_->layout.output_root) {
        auto build_root_name = pool.get(pup::path::relative(
            pool.get(ctx.impl_->layout.output_root),
            pool.get(ctx.impl_->layout.source_root)
        ));
        ctx.impl_->graph.set_build_root_name(build_root_name);
    }

    // 2. Auto-init if needed
    if (ctx_opts.auto_init) {
        try_auto_init(ctx.impl_->layout);
    }

    // 3. Discover Tupfiles
    auto ignore = load_ignore_list(ctx.impl_->layout, ctx_opts.verbose);
    ctx.impl_->state.available = discover_tupfile_dirs(pool.get(ctx.impl_->layout.config_root), ignore);

    if (ctx.impl_->state.available.empty()) {
        return make_error<BuildContext>(ErrorCode::IoError, "No Tupfiles found in project");
    }

    if (ctx_opts.verbose) {
        printf("Found %zu directories with Tupfiles\n", ctx.impl_->state.available.size());
    }

    // 4. Load config (seeds the per-file parse cache for find_config_for_dir)
    auto config_path = String { pool.get(pup::path::join(pool.get(ctx.impl_->layout.output_root), "tup.config")) };
    if (pup::platform::exists(config_path)) {
        auto const* root_cfg = get_or_parse_config(config_path, ctx.impl_->state);
        if (root_cfg) {
            ctx.impl_->config_vars = *root_cfg;
            if (ctx_opts.verbose) {
                printf("Loaded %zu config variables from %s\n", ctx.impl_->config_vars.names().size(), config_path.c_str());
            }
        }
    }

    // Apply -D config overrides (highest precedence)
    for (auto const& [name, value] : opts.config_defines) {
        auto name_sv = pool.get(name);
        auto value_sv = pool.get(value);
        ctx.impl_->config_vars.set(name_sv, value_sv);
        ctx.impl_->config_vars.set(String { "CONFIG_" } + name_sv, value_sv);
        if (ctx_opts.verbose) {
            printf("-D %s=%s\n", name_sv.data(), value_sv.data());
        }
    }

    // Store config_defines for scoped configs to use
    if (!opts.config_defines.empty()) {
        ctx.impl_->state.config_defines = &opts.config_defines;
    }

    // 5. Load index
    auto [old_index, cached_env_vars] = load_old_index(pool.get(ctx.impl_->layout.output_root), ctx_opts.verbose);
    ctx.impl_->old_index = std::move(old_index);

    // 6. Parse Tupfiles
    auto builder_opts = graph::BuilderOptions {
        .source_root = ctx.impl_->layout.source_root,
        .config_root = ctx.impl_->layout.config_root,
        .output_root = ctx.impl_->layout.output_root,
        .config_path = pool.intern(config_path),
        .expand_globs = true,
        .verbose = ctx_opts.verbose,
        .scanner_registry = ctx_opts.scanner_registry,
        .pattern_registry = ctx_opts.pattern_registry,
        .cached_env_vars = std::move(cached_env_vars),
    };
    auto builder = graph::GraphBuilder { std::move(builder_opts) };

    auto parse_ctx = ParseContext {
        .state = ctx.impl_->state,
        .builder = builder,
        .graph = ctx.impl_->graph,
        .source_root = pool.get(ctx.impl_->layout.source_root),
        .config_root = pool.get(ctx.impl_->layout.config_root),
        .output_root = pool.get(ctx.impl_->layout.output_root),
        .base_vars = ctx.impl_->vars,
        .verbose = ctx_opts.verbose,
        .root_config_only = ctx_opts.root_config_only,
        .on_var_assigned = ctx_opts.on_var_assigned,
    };

    for (auto dir_id : sort_dirs_by_depth(ctx.impl_->state.available)) {
        auto dir_sv = pool.get(dir_id);
        if (sorted_contains(ctx.impl_->state.parsed, dir_sv)) {
            continue;
        }
        if (!ctx_opts.parse_scopes.empty()
            && !pup::is_path_in_any_scope(dir_sv, ctx_opts.parse_scopes)) {
            continue;
        }
        auto result = Result<void> { parse_directory(String { dir_sv }, parse_ctx) };
        if (!result && !ctx_opts.keep_going) {
            return unexpected<Error>(result.error());
        }
    }

    // Resolve deferred order-only edges (side effect: modifies graph; return value is count, unused)
    (void)builder.resolve_deferred_order_only_edges(ctx.impl_->graph);

    for (auto warning_id : builder.warnings()) {
        fprintf(stderr, "warning: %s\n", pool.get(warning_id).data());
    }

    if (ctx_opts.verbose) {
        printf("Parsed %zu Tupfiles\n", ctx.impl_->state.parsed.size());
    }

    return ctx;
}

auto resolve_clean_context(Options const& opts) -> std::optional<CleanContext>
{
    auto& pool = global_pool();
    auto cwd = *pup::platform::current_directory();
    auto root = find_project_root(cwd);
    if (!root) {
        return std::nullopt;
    }

    auto root_sv = pool.get(*root);
    auto build_dir = String {};
    auto is_in_tree = false;

    if (!opts.build_dirs.empty()) {
        build_dir = String { pool.get(opts.build_dirs[0]) };
        if (!pup::path::is_absolute(build_dir)) {
            build_dir = String { pool.get(pup::path::join(root_sv, build_dir)) };
        }
        is_in_tree = (build_dir == root_sv);
    } else if (pup::platform::exists(pool.get(pup::path::join(cwd, ".pup"))) && cwd != root_sv) {
        build_dir = cwd;
        is_in_tree = false;
    } else if (auto detected = find_build_subdir(root_sv)) {
        build_dir = *detected;
        is_in_tree = false;
    } else if (pup::platform::exists(pool.get(pup::path::join(root_sv, "tup.config")))
               || pup::platform::exists(pool.get(pup::path::join(root_sv, ".pup")))) {
        build_dir = String { root_sv };
        is_in_tree = true;
    } else {
        return std::nullopt;
    }

    return CleanContext { *root, pool.intern(build_dir), is_in_tree };
}

} // namespace pup::cli
