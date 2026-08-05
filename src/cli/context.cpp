// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/context.hpp"
#include "pup/cli/options.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/clock.hpp"
#include "pup/core/expected.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/print.hpp"
#include "pup/core/result.hpp"
#include "pup/core/stable_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/builder.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/clang_cl.hpp"
#include "pup/graph/scanners/gcc.hpp"
#include "pup/index/reader.hpp"
#include "pup/parser/ast.hpp"
#include "pup/parser/config.hpp"
#include "pup/parser/ignore.hpp"
#include "pup/parser/parser.hpp"
#include "pup/platform/env.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace pup::cli {

auto make_scanner_registry() -> std::optional<graph::DepScannerRegistry>
{
    if (auto const* env = pup::platform::get_env("PUP_IMPLICIT_DEPS"); env && std::string_view { env } == "0") {
        return std::nullopt;
    }
    auto registry = graph::DepScannerRegistry {};
    registry.register_scanner(graph::scanners::make_gcc_scanner());
    registry.register_scanner(graph::scanners::make_clang_cl_scanner());
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
    auto cwd = pool.get(*pup::platform::current_directory());
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

auto normalize_to_empty(std::string_view p) -> std::string_view
{
    return (p.empty() || p == ".") ? std::string_view {} : p;
}

auto normalize_to_dot(std::string_view p) -> std::string_view
{
    return (p.empty() || p == ".") ? std::string_view { "." } : p;
}

auto join_path(std::string_view base, std::string_view rel) -> std::string_view
{
    return (rel.empty() || rel == ".") ? base : pup::global_pool().get(pup::path::join(base, rel));
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
    Vec<StringId> pruned;
    Vec<StringId> parsed;
    Vec<StringId> parsing;
    Vec<StringId> failed;
    std::size_t errors_printed = 0;
    // Append-only paged vectors: push_back preserves references to existing
    // elements, which is critical because recursive Tupfile parsing holds
    // VarDb pointers across calls that may insert new entries.
    StableVec<std::pair<StringId, parser::VarDb>> parsed_configs;
    StableVec<std::pair<StringId, parser::VarDb>> scoped_configs;
    Vec<std::pair<pup::StringId, pup::StringId>> const* config_defines = nullptr; // CLI overrides
};

auto compute_tup_variantdir(
    std::string_view source_dir,
    std::string_view source_root,
    std::string_view output_root
) -> std::string_view
{
    auto& pool = pup::global_pool();
    if (!output_root.empty() && source_root != output_root) {
        auto output_dir = pool.get(pup::path::join(output_root, source_dir));
        auto src_dir = pool.get(pup::path::join(source_root, source_dir));
        auto src_canonical = pup::platform::canonical(src_dir);
        auto out_canonical = pup::platform::canonical(output_dir);
        if (src_canonical && out_canonical) {
            return pool.get(pup::path::relative(pool.get(*out_canonical), pool.get(*src_canonical)));
        }
        return ".";
    }

    return ".";
}

auto read_file(std::string_view path) -> std::optional<StringId>
{
    auto content = Buf {};
    if (!pup::platform::read_file(path, content)) {
        return std::nullopt;
    }
    return content.intern(global_pool());
}

auto is_ancestor_of_any(std::string_view dir, Vec<StringId> const& paths) -> bool
{
    auto& pool = global_pool();
    for (auto path_id : paths) {
        auto path_sv = pool.get(path_id);
        if (path_sv == dir
            || (path_sv.size() > dir.size() && path_sv.starts_with(dir) && path_sv[dir.size()] == '/')) {
            return true;
        }
    }
    return false;
}

/// Discover Tupfile-containing dirs under `root`, pruning nested project roots
/// (subdirs with their own Tupfile.ini). An explicitly scoped path overrides
/// the pruning for its subtree — targeting a nested project asserts membership.
/// `prefix` is root's project-relative path ("" for the project root itself),
/// used to compare walk-relative paths against project-relative scopes.
auto discover_tupfile_dirs(
    std::string_view root,
    pup::parser::IgnoreList const& ignore = {},
    Vec<StringId> const& keep_scopes = {},
    std::string_view prefix = {},
    Vec<StringId>* pruned_roots = nullptr
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
        if (entry.is_dir
            && pup::platform::exists(pool.get(pup::path::join(pool.get(pup::path::join(root, rel_path)), "Tupfile.ini")))) {
            auto project_rel = prefix.empty() ? rel_path : pool.get(pup::path::join(prefix, rel_path));
            if (!is_ancestor_of_any(project_rel, keep_scopes)) {
                if (pruned_roots) {
                    pruned_roots->push_back(pool.intern(project_rel));
                }
                return false;
            }
        }

        if (!entry.is_dir && entry.name == "Tupfile") {
            auto dir_rel = pup::path::parent(rel_path);
            dirs.push_back(pool.intern(normalize_to_dot(dir_rel)));
        }

        return true;
    });

    std::sort(dirs.begin(), dirs.end(), pup::handle_less);
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
        auto cbuf = Buf {};
        cbuf.append("CONFIG_");
        cbuf.append(name_sv);
        config.set(cbuf.view(), value_sv);
    }
}

/// Parse a tup.config file, returning a cached result on repeat calls.
auto get_or_parse_config(
    std::string_view path,
    TupfileParseState& state
) -> parser::VarDb const*
{
    auto path_id = global_pool().intern(path);
    for (auto const& entry : state.parsed_configs) {
        if (entry.first == path_id) {
            return &entry.second;
        }
    }

    auto result = parser::parse_config(path);
    if (!result) {
        auto cp = Buf {};
        cp.append(path);
        eprint("Warning: Failed to parse {}: {}\n", cp.c_str(), result.error().msg());
        return nullptr;
    }

    state.parsed_configs.emplace_back(path_id, std::move(*result));
    return &state.parsed_configs.back().second;
}

/// Merge all tup.config files from root down to target directory.
/// Parent configs override child configs on collision (same authority
/// model as Tuprules.tup ?= defaults).
/// Returns pointer to the cached VarDb for that directory.
auto find_config_for_dir(
    std::string_view rel_dir,
    std::string_view output_root,
    TupfileParseState& state
) -> parser::VarDb const*
{
    auto normalized = normalize_to_empty(rel_dir);
    auto norm_id = global_pool().intern(normalized);

    // Check cache first
    for (auto const& entry : state.scoped_configs) {
        if (entry.first == norm_id) {
            return &entry.second;
        }
    }

    // Collect all tup.config paths from root down to target directory
    auto config_paths = Vec<StringId> {};

    auto& pool = pup::global_pool();
    auto root_config_sv = pool.get(pup::path::join(output_root, "tup.config"));
    if (pup::platform::exists(root_config_sv)) {
        config_paths.push_back(pool.intern(root_config_sv));
    }

    if (!normalized.empty()) {
        auto accumulated_id = pool.intern(output_root);
        auto remaining = normalized;
        while (!remaining.empty()) {
            auto slash = remaining.find('/');
            auto component = (slash == std::string_view::npos) ? remaining : remaining.substr(0, slash);
            remaining = (slash == std::string_view::npos) ? std::string_view {} : remaining.substr(slash + 1);
            if (component.empty()) {
                continue;
            }
            accumulated_id = pup::path::join(pool.get(accumulated_id), component);
            auto config_path_sv = pool.get(pup::path::join(pool.get(accumulated_id), "tup.config"));
            if (pup::platform::exists(config_path_sv)) {
                config_paths.push_back(pool.intern(config_path_sv));
            }
        }
    }

    if (config_paths.empty()) {
        state.scoped_configs.emplace_back(norm_id, parser::VarDb {});
        return &state.scoped_configs.back().second;
    }

    // Merge leaf first (defaults), then each parent on top (overrides).
    // config_paths is root-to-leaf, so reverse iteration gives leaf->root.
    auto merged = parser::VarDb {};
    for (auto it = config_paths.rbegin(); it != config_paths.rend(); ++it) {
        auto const* cfg = get_or_parse_config(pool.get(*it), state);
        if (cfg) {
            for (auto const& name : cfg->names()) {
                merged.set(name, cfg->get(name));
            }
        }
    }

    apply_config_overrides(merged, state.config_defines);
    state.scoped_configs.emplace_back(norm_id, std::move(merged));
    return &state.scoped_configs.back().second;
}

auto make_err(std::string_view prefix, std::string_view suffix) -> StringId
{
    auto buf = Buf {};
    buf.append(prefix);
    buf.append(suffix);
    return buf.intern(global_pool());
}

auto make_circular_dep_error(std::string_view dir) -> pup::Error
{
    return pup::Error {
        pup::ErrorCode::CyclicDependency,
        make_err("Circular Tupfile dependency: ", dir)
    };
}

auto make_read_error(std::string_view path) -> pup::Error
{
    return pup::Error {
        pup::ErrorCode::IoError,
        make_err("Failed to read ", path)
    };
}

auto make_eval_failed_error(std::string_view dir) -> pup::Error
{
    return pup::Error {
        pup::ErrorCode::ParseError,
        make_err("Tupfile evaluation failed in ", dir)
    };
}

struct ParseContext {
    TupfileParseState& state;
    pup::graph::Builder& builder_state;
    pup::graph::BuildGraph& graph;
    std::string_view source_root;
    std::string_view config_root;
    std::string_view output_root;
    pup::parser::VarDb const& base_vars;
    bool verbose;
    bool root_config_only;
    VarAssignedCallback const* on_var_assigned;
    StatementCallback const* on_statement;
};

/// Compose the nested project containing `dir` into the build: the outermost
/// ancestor of `dir` carrying its own Tupfile.ini has its subtree discovered
/// and appended to the available set. Returns true if new dirs were added.
auto compose_nested_project_subtree(std::string_view dir, ParseContext& ctx) -> bool
{
    if (dir.empty() || dir == "." || pup::path::is_absolute(dir)) {
        return false;
    }
    auto& pool = global_pool();

    auto marker_prefix = std::string_view {};
    for (auto sep = dir.find('/');; sep = dir.find('/', sep + 1)) {
        auto prefix = (sep == std::string_view::npos) ? dir : dir.substr(0, sep);
        auto prefix_abs = pool.get(pup::path::join(ctx.config_root, prefix));
        if (pup::platform::exists(pool.get(pup::path::join(prefix_abs, "Tupfile.ini")))) {
            marker_prefix = prefix;
            break;
        }
        if (sep == std::string_view::npos) {
            return false;
        }
    }

    auto subtree_root = pool.get(pup::path::join(ctx.config_root, marker_prefix));
    auto sub_dirs = discover_tupfile_dirs(
        subtree_root, pup::parser::IgnoreList::with_defaults(), {}, marker_prefix, &ctx.state.pruned
    );

    auto marker_id = pool.intern(marker_prefix);
    auto& pruned = ctx.state.pruned;
    pruned.erase(std::remove(pruned.begin(), pruned.end(), marker_id), pruned.end());

    auto& available = ctx.state.available;
    auto const before = available.size();
    for (auto sub_id : sub_dirs) {
        auto sub_sv = pool.get(sub_id);
        auto rel_id = (sub_sv == ".") ? pool.intern(marker_prefix) : pup::path::join(marker_prefix, sub_sv);
        // Search only the sorted prefix: appending unsorts the tail, and a binary search
        // over that reports a present directory absent, so it gets parsed twice.
        auto* sorted_end = available.begin() + static_cast<std::ptrdiff_t>(before);
        if (!std::binary_search(available.begin(), sorted_end, rel_id, pup::handle_less)) {
            available.push_back(rel_id);
        }
    }
    if (available.size() == before) {
        return false;
    }
    std::sort(available.begin(), available.end(), pup::handle_less);
    available.erase(std::unique(available.begin(), available.end()), available.end());
    return true;
}

auto parse_directory(std::string_view rel_dir, ParseContext& ctx) -> pup::Result<void>
{
    auto vars = pup::parser::VarDb { ctx.base_vars };
    auto normalized_dir = normalize_to_dot(rel_dir);

    if (sorted_contains(ctx.state.parsed, normalized_dir)) {
        return {};
    }

    if (sorted_contains(ctx.state.failed, normalized_dir)) {
        return pup::unexpected<pup::Error>(make_eval_failed_error(normalized_dir));
    }

    if (sorted_contains(ctx.state.parsing, normalized_dir)) {
        return pup::unexpected<pup::Error>(make_circular_dep_error(normalized_dir));
    }

    sorted_insert(ctx.state.parsing, normalized_dir);

    // Tupfiles are found in config_root (may differ from source_root in 3-tree builds)
    auto tupfile_path_sv = pup::global_pool().get(pup::path::join(join_path(ctx.config_root, normalize_to_empty(rel_dir)), "Tupfile"));

    if (ctx.verbose) {
        auto tp = Buf {};
        tp.append(tupfile_path_sv);
        print("Parsing: {}\n", tp.c_str());
    }

    auto source = read_file(tupfile_path_sv);
    if (!source) {
        sorted_erase(ctx.state.parsing, normalized_dir);
        return pup::unexpected<pup::Error>(make_read_error(tupfile_path_sv));
    }

    auto source_sv = global_pool().get(*source);
    auto parse_result = pup::parser::parse_tupfile(source_sv, tupfile_path_sv);
    if (!parse_result.success()) {
        sorted_erase(ctx.state.parsing, normalized_dir);
        auto tp = Buf {};
        tp.append(tupfile_path_sv);
        for (auto const& err : parse_result.errors) {
            eprint("{}:{}:{}: error: {}\n", tp.c_str(), err.location.line, err.location.column, global_pool().get(err.message));
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
    auto tup_srcdir = std::string_view { "." };

    // TUP_OUTDIR: relative path from source dir (where commands run) to output dir.
    // For in-tree builds (source == output): "."
    // For variant builds: e.g., "../../build/coreutils" from source/coreutils/
    auto tup_outdir = std::string_view { "." };
    if (ctx.source_root != ctx.output_root) {
        auto source_dir = pup::platform::canonical(join_path(ctx.source_root, rel_dir_normalized));
        auto output_dir = pup::platform::canonical(join_path(ctx.output_root, rel_dir_normalized));
        if (source_dir && output_dir) {
            tup_outdir = pup::global_pool().get(pup::path::relative(pup::global_pool().get(*output_dir), pup::global_pool().get(*source_dir)));
        }
    }

    // Get the scoped config for this directory (walks up tree to find nearest tup.config)
    // When root_config_only is set (for configure pass), always use root config
    auto const* scoped_config = find_config_for_dir(
        ctx.root_config_only ? std::string_view {} : rel_dir,
        ctx.output_root,
        ctx.state
    );

    auto request_directory = [&](std::string_view dir) -> pup::Result<void> {
        return parse_directory(dir, ctx);
    };

    auto compose_nested_project = [&](std::string_view dir) -> bool {
        return compose_nested_project_subtree(dir, ctx);
    };

    auto& pool = global_pool();
    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .config_vars = scoped_config,
        .tup_cwd = pool.intern(tup_cwd),
        .tup_platform = pup::get_platform(),
        .tup_arch = pool.intern(pup::ARCH),
        .tup_variantdir = pool.intern(tup_variantdir),
        .tup_variant_outputdir = pool.intern(tup_variantdir),
        .tup_srcdir = pool.intern(tup_srcdir),
        .tup_outdir = pool.intern(tup_outdir),
        .request_directory = request_directory,
        .available_tupfile_dirs = &ctx.state.available,
        .compose_nested_project = compose_nested_project,
    };

    if (ctx.on_var_assigned && *ctx.on_var_assigned) {
        auto const* cb = ctx.on_var_assigned;
        eval_ctx.on_var_assigned = [cb](
                                       std::string_view name,
                                       pup::parser::Assignment::Op op,
                                       std::string_view value_before,
                                       std::string_view value_after,
                                       std::string_view filename,
                                       std::uint32_t line,
                                       std::uint32_t column,
                                       bool is_effective
                                   ) {
            (*cb)(name, op, value_before, value_after, filename, line, column, is_effective);
        };
    }

    if (ctx.on_statement && *ctx.on_statement) {
        auto const* cb = ctx.on_statement;
        eval_ctx.on_statement = [cb](pup::parser::Statement const& stmt, std::string_view dir) {
            (*cb)(stmt, dir);
        };
    }

    auto result = pup::Result<void> { pup::graph::add_tupfile(ctx.graph, parse_result.tupfile, eval_ctx, ctx.builder_state) };

    sorted_erase(ctx.state.parsing, normalized_dir);

    if (!result) {
        // Nested demand-driven parses print their own errors; the watermark keeps each printed once.
        for (auto i = ctx.state.errors_printed; i < ctx.builder_state.errors.size(); ++i) {
            eprint("error: {}\n", global_pool().get(ctx.builder_state.errors[i]));
        }
        ctx.state.errors_printed = ctx.builder_state.errors.size();
        sorted_insert(ctx.state.failed, normalized_dir);
        return pup::unexpected<pup::Error>(make_eval_failed_error(normalized_dir));
    }

    sorted_insert(ctx.state.parsed, normalized_dir);
    ++pup::thread_metrics().tupfiles_parsed;

    return result;
}

auto try_auto_init(ProjectLayout const& layout, bool dry_run) -> void
{
    auto& pool = global_pool();
    auto pup_dir_sv = pool.get(layout.pup_dir());
    if (pup::platform::exists(pup_dir_sv)) {
        return;
    }
    if (!pup::platform::exists(pool.get(pup::path::join(pool.get(layout.source_root), "Tupfile.ini")))) {
        return;
    }
    // A dry run creates nothing, so it must not create the project either (#261).
    if (dry_run) {
        print("Would initialize pup in \"{}\"\n", pup_dir_sv);
        return;
    }
    (void)pup::platform::create_directories(pup_dir_sv);
    print("Initialized pup in \"{}\"\n", pup_dir_sv);
}

struct IndexLoadResult {
    std::optional<pup::index::Index> index;
    Vec<std::pair<StringId, StringId>> cached_env_vars;
};

auto load_old_index(std::string_view output_root, bool verbose) -> IndexLoadResult
{
    auto& pool = pup::global_pool();
    auto result = IndexLoadResult {};
    auto index_path_sv = pool.get(pup::path::join(pool.get(pup::path::join(output_root, ".pup")), "index"));

    if (!pup::platform::exists(index_path_sv)) {
        return result;
    }

    auto index_load_start = pup::SteadyClock::now();
    auto index_result = pup::index::read_index(index_path_sv);
    if (!index_result) {
        // Not fatal here -- what this build can still do depends on what is on disk, and the
        // guard that knows decides (#291). Said out loud because the alternative is a silent
        // full rebuild, which reads as a first build (#294).
        if (index_result.error().code == pup::ErrorCode::IndexChecksumMismatch) {
            eprint("Warning: the build record at {} failed its checksum, so this build cannot use it.\n", index_path_sv);
        }
        return result;
    }

    result.index = std::move(*index_result);

    auto index_load_end = pup::SteadyClock::now();
    pup::thread_metrics().index_load_time = std::chrono::duration_cast<std::chrono::microseconds>(
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

    std::ranges::sort(result.cached_env_vars, {}, [&pool](auto const& p) { return pool.get(p.first); });

    if (verbose && !result.cached_env_vars.empty()) {
        print("Loaded {} cached env vars from index\n", result.cached_env_vars.size());
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

} // namespace

auto make_exclude_list(Options const& opts) -> parser::IgnoreList
{
    auto excludes = parser::IgnoreList {};
    for (auto pattern_id : opts.excludes) {
        excludes.add(global_pool().get(pattern_id));
    }
    return excludes;
}

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
    graph::BuildGraph graph = graph::make_build_graph();
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

auto BuildContext::available_dirs() const -> Vec<StringId> const&
{
    return impl_->state.available;
}

auto BuildContext::pruned_dirs() const -> Vec<StringId> const&
{
    return impl_->state.pruned;
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
        graph::set_build_root_name(ctx.impl_->graph, build_root_name);
    }

    // 2. Auto-init if needed
    if (ctx_opts.auto_init) {
        try_auto_init(ctx.impl_->layout, ctx_opts.dry_run);
    }

    // 3. Discover Tupfiles
    ctx.impl_->state.available = discover_tupfile_dirs(
        pool.get(ctx.impl_->layout.config_root),
        pup::parser::IgnoreList::with_defaults(),
        ctx_opts.parse_scopes,
        {},
        &ctx.impl_->state.pruned
    );

    if (ctx.impl_->state.available.empty()) {
        return make_error<BuildContext>(ErrorCode::IoError, "No Tupfiles found in project");
    }

    if (ctx_opts.verbose) {
        print("Found {} directories with Tupfiles\n", ctx.impl_->state.available.size());
    }

    // 4. Load config (seeds the per-file parse cache for find_config_for_dir)
    auto config_path_sv = pool.get(pup::path::join(pool.get(ctx.impl_->layout.output_root), "tup.config"));
    if (pup::platform::exists(config_path_sv)) {
        auto const* root_cfg = get_or_parse_config(config_path_sv, ctx.impl_->state);
        if (root_cfg) {
            ctx.impl_->config_vars = *root_cfg;
            if (ctx_opts.verbose) {
                auto cpbuf = Buf {};
                cpbuf.append(config_path_sv);
                print("Loaded {} config variables from {}\n", ctx.impl_->config_vars.names().size(), cpbuf.c_str());
            }
        }
    }

    // Apply -D config overrides (highest precedence)
    for (auto const& [name, value] : opts.config_defines) {
        auto name_sv = pool.get(name);
        auto value_sv = pool.get(value);
        ctx.impl_->config_vars.set(name_sv, value_sv);
        auto cbuf = Buf {};
        cbuf.append("CONFIG_");
        cbuf.append(name_sv);
        ctx.impl_->config_vars.set(cbuf.view(), value_sv);
        if (ctx_opts.verbose) {
            print("-D {}={}\n", name_sv, value_sv);
        }
    }

    // Store config_defines for scoped configs to use
    if (!opts.config_defines.empty()) {
        ctx.impl_->state.config_defines = &opts.config_defines;
    }

    // 5. Load index
    auto [old_index, cached_env_vars] = load_old_index(pool.get(ctx.impl_->layout.output_root), ctx_opts.verbose);
    ctx.impl_->old_index = std::move(old_index);

    // 6. Parse Tupfiles.
    //
    // A rule may glob over files a directory parsed later generates, so a single pass
    // resolves that glob against a partial graph. Each round records what it generated
    // and seeds the next, until no glob's match set changes (issue #188). The rule set
    // is a function of the source tree alone — no command runs during parsing — and the
    // seed only grows, so this converges; two rounds is the usual worst case.
    auto constexpr max_parse_rounds = 8;
    auto generated_seed = Vec<StringId> {};
    auto builder_state = graph::Builder {};

    for (auto round = 0; round < max_parse_rounds; ++round) {
        auto builder_opts = graph::BuilderOptions {
            .source_root = ctx.impl_->layout.source_root,
            .config_root = ctx.impl_->layout.config_root,
            .output_root = ctx.impl_->layout.output_root,
            .config_path = pool.intern(config_path_sv),
            .expand_globs = true,
            .verbose = ctx_opts.verbose,
            .reject_empty_commands = !ctx_opts.root_config_only,
            .scanner_registry = ctx_opts.scanner_registry,
            .pattern_registry = ctx_opts.pattern_registry,
            .cached_env_vars = cached_env_vars,
            .generated_seed = generated_seed,
        };
        builder_state = graph::make_builder(std::move(builder_opts));

        auto parse_ctx = ParseContext {
            .state = ctx.impl_->state,
            .builder_state = builder_state,
            .graph = ctx.impl_->graph,
            .source_root = pool.get(ctx.impl_->layout.source_root),
            .config_root = pool.get(ctx.impl_->layout.config_root),
            .output_root = pool.get(ctx.impl_->layout.output_root),
            .base_vars = ctx.impl_->vars,
            .verbose = ctx_opts.verbose,
            .root_config_only = ctx_opts.root_config_only,
            .on_var_assigned = &ctx_opts.on_var_assigned,
            .on_statement = &ctx_opts.on_statement,
        };

        // Parse to a fixpoint: a group reference under a nested project root
        // composes that subtree into the available set mid-pass, and the new
        // dirs need a pass of their own.
        while (true) {
            auto const available_before = ctx.impl_->state.available.size();
            for (auto dir_id : sort_dirs_by_depth(ctx.impl_->state.available)) {
                auto dir_sv = pool.get(dir_id);
                if (sorted_contains(ctx.impl_->state.parsed, dir_sv)) {
                    continue;
                }
                if (!ctx_opts.parse_scopes.empty()
                    && !pup::is_path_in_any_scope(dir_sv, ctx_opts.parse_scopes)) {
                    continue;
                }
                if (ctx_opts.excludes.is_ignored_dir(dir_sv)) {
                    continue;
                }
                auto result = parse_directory(dir_sv, parse_ctx);
                if (!result && !ctx_opts.keep_going) {
                    return unexpected<Error>(result.error());
                }
            }
            if (ctx.impl_->state.available.size() == available_before) {
                break;
            }
        }

        // The configure pass parses only the root config, so a partial match set there
        // is intended rather than a traversal artefact.
        if (ctx_opts.root_config_only) {
            break;
        }

        auto stable = graph::check_glob_stability(ctx.impl_->graph, builder_state);
        if (stable) {
            break;
        }
        if (round + 1 == max_parse_rounds) {
            return unexpected<Error>(stable.error());
        }

        generated_seed = graph::collect_generated_paths(ctx.impl_->graph);

        auto build_root_name = pool.intern(graph::get_build_root_name(ctx.impl_->graph.graph));
        ctx.impl_->graph = graph::make_build_graph();
        graph::set_build_root_name(ctx.impl_->graph, pool.get(build_root_name));
        ctx.impl_->state.parsed.clear();
        ctx.impl_->state.parsing.clear();
        ctx.impl_->state.failed.clear();
    }

    auto finalized = graph::finalize_graph(ctx.impl_->graph, builder_state);
    if (!finalized) {
        return unexpected<Error>(finalized.error());
    }

    for (auto warning_id : builder_state.warnings) {
        eprint("warning: {}\n", pool.get(warning_id));
    }

    if (ctx_opts.verbose) {
        print("Parsed {} Tupfiles\n", ctx.impl_->state.parsed.size());
    }

    return ctx;
}

auto resolve_clean_context(Options const& opts) -> std::optional<CleanContext>
{
    auto& pool = global_pool();
    auto cwd_id = *pup::platform::current_directory();
    auto cwd = pool.get(cwd_id);
    auto root = find_project_root(cwd);
    if (!root) {
        return std::nullopt;
    }

    auto root_sv = pool.get(*root);
    auto build_dir_id = StringId::Empty;
    auto is_in_tree = false;

    if (!opts.build_dirs.empty()) {
        auto bd_sv = pool.get(opts.build_dirs[0]);
        build_dir_id = pup::path::is_absolute(bd_sv) ? opts.build_dirs[0] : pup::path::join(root_sv, bd_sv);
        is_in_tree = (pool.get(build_dir_id) == root_sv);
    } else if (pup::platform::exists(pool.get(pup::path::join(cwd, ".pup"))) && cwd != root_sv) {
        build_dir_id = cwd_id;
        is_in_tree = false;
    } else if (pup::platform::exists(pool.get(pup::path::join(root_sv, "tup.config")))
               || pup::platform::exists(pool.get(pup::path::join(root_sv, ".pup")))) {
        build_dir_id = pool.intern(root_sv);
        is_in_tree = true;
    } else {
        return std::nullopt;
    }

    return CleanContext { *root, build_dir_id, is_in_tree };
}

} // namespace pup::cli
