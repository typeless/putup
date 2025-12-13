// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/core/hash.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/core/types.hpp"
#include "pup/exec/runner.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/builder.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/graph/topo.hpp"
#include "pup/index/reader.hpp"
#include "pup/index/writer.hpp"
#include "pup/parser/config.hpp"
#include "pup/parser/ignore.hpp"
#include "pup/parser/parser.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>

#include <fmt/core.h>

namespace {

auto const VERSION = "0.1.0";
auto const PUP_DIR = ".pup";

struct Options {
    std::size_t jobs = 0;
    bool keep_going = false;
    bool verbose = false;
    bool dry_run = false;
    bool version = false;
    bool help = false;
    bool summary = false; ///< --summary flag for export graph
    std::string command = {};
    std::string export_format = {}; ///< Format for export command (script, compdb, graph)
    std::string source_dir = {};    ///< -S: source directory
    std::string build_dir = {};     ///< -B: build/output directory
    std::vector<std::string> targets = {};
};

auto print_usage() -> void
{
    fmt::print("pup - Tup build system reimplementation\n\n"
               "Usage: pup [OPTIONS] [COMMAND]\n\n"
               "Commands:\n"
               "  init              Initialize .pup directory\n"
               "  parse             Parse and validate Tupfiles\n"
               "  build             Execute build (default)\n"
               "  export <format>   Export build info:\n"
               "                      script  - Shell script\n"
               "                      compdb  - compile_commands.json\n"
               "                      graph   - DOT format (--summary for text)\n"
               "  clean             Remove generated files\n"
               "  distclean         Full reset: remove .pup and variant directory\n"
               "  variant <config> [dir]  Create variant build directory\n"
               "\nOptions:\n"
               "  -j, --jobs N       Run N jobs in parallel\n"
               "  -k, --keep-going   Continue after failures\n"
               "  -n, --dry-run      Print commands without executing\n"
               "  -v, --verbose      Verbose output\n"
               "  -S DIR             Source directory (default: auto-detect)\n"
               "  -B DIR             Build/output directory (default: source)\n"
               "  --summary          Human-readable output (for export graph)\n"
               "  --version          Print version\n"
               "  -h, --help         Print this help\n"
               "\nEnvironment:\n"
               "  PUP_SOURCE_DIR     Source directory (overridden by -S)\n"
               "  PUP_BUILD_DIR      Build directory (overridden by -B)\n"
               "  PUP_IMPLICIT_DEPS  Set to 0 to disable auto-generated dep rules (default: enabled)\n");
}

auto print_version() -> void
{
    fmt::print("pup {}\n", VERSION);
    fmt::print("Platform: {}\n", pup::PLATFORM);
    fmt::print("Architecture: {}\n", pup::ARCH);
}

auto parse_args(int argc, char** argv) -> Options
{
    auto opts = Options {};

    for (auto i = 1; i < argc; ++i) {
        auto arg = std::string_view { argv[i] };

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
        } else if (arg == "--version") {
            opts.version = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-n" || arg == "--dry-run") {
            opts.dry_run = true;
        } else if (arg == "-k" || arg == "--keep-going") {
            opts.keep_going = true;
        } else if (arg == "-j" || arg == "--jobs") {
            if (i + 1 < argc) {
                try {
                    opts.jobs = static_cast<std::size_t>(std::stoi(argv[++i]));
                } catch (std::exception const&) {
                    fmt::print(stderr, "Error: Invalid job count '{}'\n", argv[i]);
                    std::exit(EXIT_FAILURE);
                }
            }
        } else if (arg.starts_with("-j")) {
            try {
                opts.jobs = static_cast<std::size_t>(std::stoi(std::string { arg.substr(2) }));
            } catch (std::exception const&) {
                fmt::print(stderr, "Error: Invalid job count '{}'\n", arg.substr(2));
                std::exit(EXIT_FAILURE);
            }
        } else if (arg == "-S") {
            if (i + 1 < argc)
                opts.source_dir = std::string { argv[++i] };
        } else if (arg == "-B") {
            if (i + 1 < argc)
                opts.build_dir = std::string { argv[++i] };
        } else if (arg == "--summary") {
            opts.summary = true;
        } else if (!arg.starts_with("-")) {
            if (opts.command.empty()) {
                opts.command = std::string { arg };
            } else if (opts.command == "export" && opts.export_format.empty()) {
                opts.export_format = std::string { arg };
            } else {
                opts.targets.emplace_back(arg);
            }
        }
    }

    if (opts.command.empty())
        opts.command = "build";

    return opts;
}

/// Compute TUP_VARIANTDIR/TUP_VARIANT_OUTPUTDIR for a given source directory.
/// For in-tree builds: relative path from source_dir to variant_dir/source_dir
/// For out-of-tree builds (-B): relative path from source_dir to output_root/source_dir
///
/// Example: source_dir="modules/vpw", variant_dir="build-s1f3"
///          returns "../../build-s1f3/modules/vpw"
auto compute_variantdir(
    std::filesystem::path const& source_dir,
    std::filesystem::path const& variant_dir,
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root) -> std::string
{
    // Out-of-tree build with -B
    if (!output_root.empty() && source_root != output_root) {
        auto output_dir = output_root / source_dir;
        auto src_dir = source_root / source_dir;
        auto rel = std::filesystem::relative(output_dir, src_dir);
        return rel.string();
    }

    // In-tree variant build
    if (!variant_dir.empty()) {
        auto output_dir = std::filesystem::path { variant_dir / source_dir };
        auto rel = std::filesystem::relative(output_dir, source_dir);
        return rel.string();
    }

    return ".";
}

auto read_file(std::filesystem::path const& path) -> std::optional<std::string>
{
    auto file = std::ifstream { path };
    if (!file)
        return std::nullopt;

    auto ss = std::stringstream {};
    ss << file.rdbuf();
    return ss.str();
}

auto get_file_mtime(std::filesystem::path const& path) -> pup::FileTime
{
    struct stat st = {};
    if (::stat(path.c_str(), &st) < 0)
        return {};
    return pup::FileTime {
        .seconds = st.st_mtim.tv_sec,
        .nanoseconds = static_cast<std::int32_t>(st.st_mtim.tv_nsec),
    };
}

auto resolve_path(
    std::filesystem::path const& path,
    std::filesystem::path const& root) -> std::filesystem::path
{
    return path.is_absolute() ? path : root / path;
}

auto is_path_under_root(
    std::filesystem::path const& path,
    std::filesystem::path const& root) -> bool
{
    auto path_str = path.string();
    auto root_str = root.string();
    if (!root_str.empty() && root_str.back() != '/')
        root_str += '/';
    return path_str.starts_with(root_str) || path == root;
}

// =============================================================================
// Multi-directory Tupfile parsing
// =============================================================================

/// State for tracking Tupfile parsing across multiple directories
struct TupfileParseState {
    std::set<std::filesystem::path> available; ///< Dirs with Tupfiles
    std::set<std::filesystem::path> parsed;    ///< Already processed
    std::set<std::filesystem::path> parsing;   ///< Currently processing (for cycle detection)
};

/// Discover all directories containing Tupfiles
auto discover_tupfile_dirs(std::filesystem::path const& root,
    pup::parser::IgnoreList const& ignore = {})
    -> std::set<std::filesystem::path>
{
    auto dirs = std::set<std::filesystem::path> {};
    auto ec = std::error_code {};
    auto options = std::filesystem::directory_options::skip_permission_denied;

    for (auto it = std::filesystem::recursive_directory_iterator(root, options, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec)
            break;

        auto const& entry = *it;
        auto rel = std::filesystem::relative(entry.path(), root);

        // Check if this directory should be ignored
        if (entry.is_directory() && ignore.is_ignored(rel)) {
            it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file())
            continue;
        if (entry.path().filename() != "Tupfile")
            continue;

        auto dir = std::filesystem::path { entry.path().parent_path() };

        // Skip variant directories (contain tup.config)
        if (std::filesystem::exists(dir / "tup.config"))
            continue;

        // Store relative path from root
        auto dir_rel = std::filesystem::relative(dir, root);
        dirs.insert(dir_rel.empty() || dir_rel == "." ? std::filesystem::path { "." } : dir_rel);
    }

    return dirs;
}

/// Recursive directory parsing function with cycle detection
/// Each directory gets its own copy of base_vars so variable changes don't leak between Tupfiles
auto parse_directory(
    std::filesystem::path const& rel_dir,
    TupfileParseState& state,
    pup::graph::GraphBuilder& builder,
    pup::graph::BuildGraph& graph,
    std::filesystem::path const& root,
    std::filesystem::path const& output_root,
    pup::parser::VarDb const& base_vars,
    pup::parser::VarDb const& config_vars,
    std::filesystem::path const& variant_dir,
    bool verbose) -> pup::Result<void>;

/// Implementation of parse_directory (declared above for recursive callback)
auto parse_directory(
    std::filesystem::path const& rel_dir,
    TupfileParseState& state,
    pup::graph::GraphBuilder& builder,
    pup::graph::BuildGraph& graph,
    std::filesystem::path const& root,
    std::filesystem::path const& output_root,
    pup::parser::VarDb const& base_vars,
    pup::parser::VarDb const& config_vars,
    std::filesystem::path const& variant_dir,
    bool verbose) -> pup::Result<void>
{
    // Each directory gets its own isolated variable scope
    auto vars = pup::parser::VarDb { base_vars };
    // Normalize directory path
    auto normalized_dir = std::filesystem::path {
        rel_dir.empty() || rel_dir == "." ? std::filesystem::path { "." } : rel_dir
    };

    // Skip if already parsed
    if (state.parsed.contains(normalized_dir))
        return {};

    // Circular dependency check
    if (state.parsing.contains(normalized_dir)) {
        return pup::make_error<void>(
            pup::ErrorCode::CyclicDependency,
            fmt::format("Circular Tupfile dependency: {}", normalized_dir.string()));
    }

    // Mark as currently parsing
    state.parsing.insert(normalized_dir);

    // Build the Tupfile path
    auto tupfile_path = std::filesystem::path {
        normalized_dir == "." ? root / "Tupfile" : root / rel_dir / "Tupfile"
    };

    if (verbose)
        fmt::print("Parsing: {}\n", tupfile_path.string());

    auto source = read_file(tupfile_path);
    if (!source) {
        state.parsing.erase(normalized_dir);
        return pup::make_error<void>(
            pup::ErrorCode::IoError,
            fmt::format("Failed to read {}", tupfile_path.string()));
    }

    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto parse_result = pup::Result<pup::parser::Tupfile> { parser.parse() };
    if (!parse_result) {
        state.parsing.erase(normalized_dir);
        return pup::unexpected<pup::Error>(parse_result.error());
    }

    // Create per-directory eval context
    auto tup_cwd = std::string { normalized_dir == "." ? "." : rel_dir.string() };
    auto tup_variantdir = compute_variantdir(
        normalized_dir == "." ? std::filesystem::path {} : rel_dir,
        variant_dir, root, output_root);
    // Create recursive callback for demand-driven parsing
    // Pass base_vars (not the local vars copy) so each directory starts fresh
    auto request_directory = [&](std::filesystem::path const& dir) -> pup::Result<void> {
        return parse_directory(dir, state, builder, graph, root, output_root, base_vars, config_vars, variant_dir, verbose);
    };

    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .config_vars = const_cast<pup::parser::VarDb*>(&config_vars),
        .tup_cwd = tup_cwd,
        .tup_platform = std::string { pup::PLATFORM },
        .tup_arch = std::string { pup::ARCH },
        .tup_variantdir = tup_variantdir,
        .tup_variant_outputdir = tup_variantdir,
        .request_directory = request_directory,
        .available_tupfile_dirs = &state.available,
    };

    // Process this Tupfile - callback may be invoked during expansion
    auto result = pup::Result<void> { builder.add_tupfile(graph, *parse_result, eval_ctx) };

    // Mark as parsed (not parsing anymore)
    state.parsing.erase(normalized_dir);
    state.parsed.insert(normalized_dir);

    return result;
}

// =============================================================================
// Unified graph building
// =============================================================================

/// Options for build_graph()
struct BuildGraphOptions {
    bool verbose = false;
    bool keep_going = false;
    bool auto_init = false;
    pup::graph::RulePatternRegistry* pattern_registry = nullptr;
};

/// Result of build_graph() containing all state needed by commands
struct BuildGraphResult {
    pup::ProjectLayout layout;
    pup::parser::VarDb config_vars;
    pup::parser::VarDb vars;
    pup::graph::BuildGraph graph;
    TupfileParseState state;
};

/// Build the dependency graph from Tupfiles
/// This is the shared implementation for graph, build, and compdb commands
auto build_graph(Options const& opts, BuildGraphOptions const& graph_opts)
    -> pup::Result<BuildGraphResult>
{
    // Discover project layout (source/output directories)
    auto layout_opts = pup::LayoutOptions {};
    if (!opts.source_dir.empty())
        layout_opts.source_dir = std::filesystem::path { opts.source_dir };
    if (!opts.build_dir.empty())
        layout_opts.build_dir = std::filesystem::path { opts.build_dir };

    auto layout_result = pup::Result<pup::ProjectLayout> { pup::discover_layout(layout_opts) };
    if (!layout_result)
        return pup::unexpected<pup::Error>(layout_result.error());

    auto layout = pup::ProjectLayout { std::move(*layout_result) };

    // Auto-initialize if requested and Tupfile.ini exists but .pup/ doesn't
    if (graph_opts.auto_init) {
        auto pup_dir = layout.pup_dir();
        if (!std::filesystem::exists(pup_dir)) {
            if (std::filesystem::exists(layout.source_root / "Tupfile.ini")) {
                std::filesystem::create_directories(pup_dir);
                fmt::print("Initialized pup in \"{}\"\n", pup_dir.string());
            }
        }
    }

    // Load ignore patterns from .pupignore (or use defaults)
    auto ignore = pup::parser::IgnoreList::with_defaults();
    auto ignore_path = layout.source_root / ".pupignore";
    if (std::filesystem::exists(ignore_path)) {
        auto ignore_result = pup::parser::IgnoreList::load(ignore_path);
        if (ignore_result) {
            ignore = std::move(*ignore_result);
            if (graph_opts.verbose)
                fmt::print("Loaded {} ignore patterns from {}\n",
                    ignore.size(), ignore_path.string());
        }
    }

    // Discover all directories containing Tupfiles
    auto state = TupfileParseState {};
    state.available = discover_tupfile_dirs(layout.source_root, ignore);

    if (state.available.empty()) {
        return pup::make_error<BuildGraphResult>(
            pup::ErrorCode::IoError, "No Tupfiles found in project");
    }

    if (graph_opts.verbose)
        fmt::print("Found {} directories with Tupfiles\n", state.available.size());

    auto variant_dir = layout.variant_dir;

    // Load config variables from tup.config
    auto config_vars = pup::parser::VarDb {};
    auto config_path = std::filesystem::path {};
    if (!opts.build_dir.empty()) {
        // -B was specified - config is directly in output_root
        config_path = layout.output_root / "tup.config";
    } else if (!variant_dir.empty()) {
        // variant was specified via --variant
        config_path = layout.source_root / variant_dir / "tup.config";
    }

    if (!config_path.empty() && std::filesystem::exists(config_path)) {
        auto config_result = pup::Result<pup::parser::VarDb> { pup::parser::parse_config(config_path) };
        if (config_result) {
            config_vars = std::move(*config_result);
            if (graph_opts.verbose)
                fmt::print("Loaded {} config variables from {}\n",
                    config_vars.names().size(), config_path.string());
        }
    }

    auto vars = pup::parser::VarDb {};

    auto builder_opts = pup::graph::BuilderOptions {
        .source_root = layout.source_root,
        .output_root = layout.output_root,
        .variant_dir = variant_dir,
        .config_path = config_path,
        .expand_globs = true,
        .pattern_registry = graph_opts.pattern_registry,
    };

    auto builder = pup::graph::GraphBuilder { builder_opts };
    auto graph = pup::graph::BuildGraph {};

    // Parse all Tupfiles starting with root
    auto root_rel = std::filesystem::path { "." };
    if (state.available.contains(root_rel)) {
        auto result = pup::Result<void> {
            parse_directory(root_rel, state, builder, graph, layout.source_root,
                layout.output_root, vars, config_vars, variant_dir, graph_opts.verbose)
        };
        if (!result && !graph_opts.keep_going)
            return pup::unexpected<pup::Error>(result.error());
    }

    // Parse remaining directories (orphan Tupfiles)
    for (auto const& dir : state.available) {
        if (state.parsed.contains(dir))
            continue;
        auto result = pup::Result<void> {
            parse_directory(dir, state, builder, graph, layout.source_root,
                layout.output_root, vars, config_vars, variant_dir, graph_opts.verbose)
        };
        if (!result && !graph_opts.keep_going)
            return pup::unexpected<pup::Error>(result.error());
    }

    if (graph_opts.verbose)
        fmt::print("Parsed {} Tupfiles\n", state.parsed.size());

    return BuildGraphResult {
        .layout = std::move(layout),
        .config_vars = std::move(config_vars),
        .vars = std::move(vars),
        .graph = std::move(graph),
        .state = std::move(state),
    };
}

// =============================================================================
// Incremental build support
// =============================================================================

auto find_changed_files_with_implicit(
    std::filesystem::path const& root,
    pup::index::Index const& old_index,
    bool verbose = false) -> std::vector<std::string>
{
    auto changed = std::vector<std::string> {};

    for (auto const& file : old_index.files()) {
        if (file.type != pup::NodeType::File && file.type != pup::NodeType::Generated)
            continue;

        auto path = resolve_path(file.path, root);

        struct stat st = {};
        if (::stat(path.c_str(), &st) < 0) {
            if (verbose)
                fmt::print("  Changed (stat failed): {}\n", file.path);
            changed.push_back(file.path);
            continue;
        }

        auto current_mtime = pup::FileTime {
            .seconds = st.st_mtim.tv_sec,
            .nanoseconds = static_cast<std::int32_t>(st.st_mtim.tv_nsec),
        };

        if (current_mtime != file.mtime) {
            if (verbose)
                fmt::print("  Changed (mtime): {} - stored {}:{} vs current {}:{}\n",
                    file.path, file.mtime.seconds, file.mtime.nanoseconds,
                    current_mtime.seconds, current_mtime.nanoseconds);
            changed.push_back(file.path);
            continue;
        }

        auto current_size = static_cast<std::uint64_t>(st.st_size);
        if (current_size != file.size) {
            if (verbose)
                fmt::print("  Changed (size): {}\n", file.path);
            changed.push_back(file.path);
            continue;
        }

        if (file.content_hash != pup::ZERO_HASH) {
            auto hash_result = pup::sha256_file(path);
            if (!hash_result || *hash_result != file.content_hash) {
                if (verbose)
                    fmt::print("  Changed (hash): {}\n", file.path);
                changed.push_back(file.path);
            }
        }
    }

    return changed;
}

auto expand_implicit_deps(
    std::vector<std::string> const& changed,
    pup::index::Index const& index,
    pup::graph::BuildGraph const& graph) -> std::vector<std::string>
{
    auto result = std::vector<std::string> { changed };
    auto added = std::set<std::string> { changed.begin(), changed.end() };

    auto path_to_file = std::unordered_map<std::string, pup::index::FileEntry const*> {};
    for (auto const& file : index.files())
        path_to_file[file.path] = &file;

    for (auto const& path : changed) {
        auto it = path_to_file.find(path);
        if (it == path_to_file.end())
            continue;

        auto file_id = pup::NodeId { it->second->id };

        for (auto const& edge : index.edges()) {
            if (edge.from != file_id)
                continue;

            // Handle implicit deps (header -> command)
            // When a header changes, mark the command's OUTPUTS as changed
            // This triggers the command to re-run without affecting unrelated commands
            if (edge.type == pup::LinkType::Implicit) {
                auto cmd_id = pup::NodeId { edge.to };
                auto const* cmd = index.find_command_by_id(cmd_id);
                if (!cmd)
                    continue;

                auto cmd_node_id = graph.find_by_command(cmd->command);
                if (!cmd_node_id)
                    continue;

                for (auto output_id : graph.get_outputs(*cmd_node_id)) {
                    auto output_path = graph.get_full_path(output_id);
                    if (!output_path.empty()) {
                        if (added.insert(output_path).second)
                            result.push_back(output_path);
                    }
                }
            }

            // Handle sticky deps (Tupfile/Tuprules -> command)
            if (edge.type == pup::LinkType::Sticky) {
                auto cmd_id = pup::NodeId { edge.to };
                auto const* cmd = index.find_command_by_id(cmd_id);
                if (!cmd)
                    continue;

                auto cmd_node_id = graph.find_by_command(cmd->command);
                if (!cmd_node_id)
                    continue;

                // Mark command's outputs as changed to trigger rebuild
                for (auto output_id : graph.get_outputs(*cmd_node_id)) {
                    auto output_path = graph.get_full_path(output_id);
                    if (!output_path.empty()) {
                        if (added.insert(output_path).second)
                            result.push_back(output_path);
                    }
                }
            }
        }
    }

    return result;
}

auto build_index(
    pup::graph::BuildGraph const& graph,
    std::unordered_map<pup::NodeId, std::vector<std::string>> const& discovered_deps,
    std::filesystem::path const& root,
    pup::index::Index const* old_index = nullptr) -> pup::index::Index
{
    auto index = pup::index::Index {};
    auto path_to_id = std::unordered_map<std::string, pup::NodeId> {};

    auto max_id = pup::NodeId { 0 };
    for (auto const& node : graph) {
        if (node.id > max_id)
            max_id = node.id;

        if (node.type == pup::NodeType::File || node.type == pup::NodeType::Generated) {
            auto node_path = graph.get_full_path(node.id);
            if (node_path.empty())
                continue;

            auto file_path = std::filesystem::path { root / node_path };
            auto content_hash = pup::Hash256 {};
            auto file_size = std::uint64_t { 0 };

            if (std::filesystem::exists(file_path)) {
                auto hash_result = pup::sha256_file(file_path);
                if (hash_result)
                    content_hash = *hash_result;

                auto ec = std::error_code {};
                file_size = std::filesystem::file_size(file_path, ec);
            }

            auto entry = pup::index::FileEntry {
                .id = node.id,
                .parent_id = node.parent_dir,
                .src_id = 0,
                .type = node.type,
                .flags = node.flags,
                .name = node.name,
                .path = node_path,
                .size = file_size,
                .mtime = get_file_mtime(file_path),
                .content_hash = content_hash,
            };
            index.add_file(std::move(entry));
            path_to_id[node_path] = node.id;
        } else if (node.type == pup::NodeType::Directory || node.type == pup::NodeType::GeneratedDir) {
            auto node_path = graph.get_full_path(node.id);
            auto entry = pup::index::FileEntry {
                .id = node.id,
                .parent_id = node.parent_dir,
                .src_id = 0,
                .type = node.type,
                .flags = node.flags,
                .name = node.name,
                .path = node_path,
                .size = 0,
                .mtime = {},
                .content_hash = {},
            };
            index.add_file(std::move(entry));
            if (!node_path.empty())
                path_to_id[node_path] = node.id;
        } else if (node.type == pup::NodeType::Command) {
            auto entry = pup::index::CommandEntry {
                .id = node.id,
                .dir_id = node.parent_dir,
                .command = node.command,
                .display = node.display,
                .env = {},
                .flags = 0,
            };
            index.add_command(std::move(entry));
        }
    }

    for (auto const& edge : graph.edges()) {
        index.add_edge(pup::index::EdgeEntry {
            .from = edge.from,
            .to = edge.to,
            .type = edge.type,
            .group_cmd_id = edge.group_cmd_id,
        });
    }

    auto next_id = pup::NodeId { max_id + 1 };
    auto added_edges = std::set<std::pair<pup::NodeId, pup::NodeId>> {};

    // Helper to get or create directory entry in index
    // For external paths (starting with /), creates a virtual "/" directory under root
    auto get_or_create_dir = [&](this auto& self, std::filesystem::path const& dir_path) -> pup::NodeId {
        auto normalized = dir_path.lexically_normal();
        auto path_str = normalized.string();

        // Project root or empty returns 0
        if (path_str.empty() || path_str == ".")
            return pup::NodeId { 0 };

        // Check if already exists
        if (auto it = path_to_id.find(path_str); it != path_to_id.end())
            return it->second;

        // Filesystem root "/" gets a virtual directory node (like tup's slash_dt)
        if (path_str == "/") {
            auto dir_id = next_id++;
            auto entry = pup::index::FileEntry {
                .id = dir_id,
                .parent_id = pup::NodeId { 0 },
                .src_id = 0,
                .type = pup::NodeType::Directory,
                .flags = pup::NodeFlags::None,
                .name = "/",
                .path = "/",
                .size = 0,
                .mtime = {},
                .content_hash = {},
            };
            index.add_file(std::move(entry));
            path_to_id["/"] = dir_id;
            return dir_id;
        }

        // Get or create parent
        auto parent_path = normalized.parent_path();
        auto parent_id = self(parent_path);
        auto basename = normalized.filename().string();

        // Create directory entry
        auto dir_id = next_id++;
        auto entry = pup::index::FileEntry {
            .id = dir_id,
            .parent_id = parent_id,
            .src_id = 0,
            .type = pup::NodeType::Directory,
            .flags = pup::NodeFlags::None,
            .name = basename,
            .path = path_str,
            .size = 0,
            .mtime = {},
            .content_hash = {},
        };
        index.add_file(std::move(entry));
        path_to_id[path_str] = dir_id;
        return dir_id;
    };

    for (auto const& [cmd_id, deps] : discovered_deps) {
        for (auto const& dep_path : deps) {
            auto abs_path = resolve_path(dep_path, root);

            auto rel_path = std::string {};
            if (is_path_under_root(abs_path, root))
                rel_path = std::filesystem::relative(abs_path, root).string();
            else
                rel_path = abs_path.string();

            auto dep_id = pup::NodeId { 0 };
            auto it = path_to_id.find(rel_path);
            if (it != path_to_id.end()) {
                dep_id = it->second;
            } else {
                dep_id = next_id++;

                auto content_hash = pup::Hash256 {};
                auto file_size = std::uint64_t { 0 };
                if (std::filesystem::exists(abs_path)) {
                    auto hash_result = pup::sha256_file(abs_path);
                    if (hash_result)
                        content_hash = *hash_result;

                    auto ec = std::error_code {};
                    file_size = std::filesystem::file_size(abs_path, ec);
                }

                // Get or create parent directory chain
                auto fs_path = std::filesystem::path { rel_path };
                auto parent_id = get_or_create_dir(fs_path.parent_path());
                auto basename = fs_path.filename().string();

                auto entry = pup::index::FileEntry {
                    .id = dep_id,
                    .parent_id = parent_id,
                    .src_id = 0,
                    .type = pup::NodeType::File,
                    .flags = pup::NodeFlags::None,
                    .name = basename,
                    .path = rel_path,
                    .size = file_size,
                    .mtime = get_file_mtime(abs_path),
                    .content_hash = content_hash,
                };
                index.add_file(std::move(entry));
                path_to_id[rel_path] = dep_id;
            }

            auto edge_key = std::pair { dep_id, cmd_id };
            if (added_edges.insert(edge_key).second) {
                index.add_edge(pup::index::EdgeEntry {
                    .from = dep_id,
                    .to = cmd_id,
                    .type = pup::LinkType::Implicit,
                    .group_cmd_id = 0,
                });
            }
        }
    }

    // Preserve implicit edges from old_index for commands that didn't run this build
    if (old_index) {
        // Build set of commands that ran this build (have new discovered deps)
        auto commands_with_new_deps = std::set<pup::NodeId> {};
        for (auto const& [cmd_id, _] : discovered_deps)
            commands_with_new_deps.insert(cmd_id);

        // Build mapping from old file paths to new file IDs
        auto old_path_to_new_id = std::unordered_map<std::string, pup::NodeId> {};
        for (auto const& file : old_index->files()) {
            auto it = path_to_id.find(file.path);
            if (it != path_to_id.end())
                old_path_to_new_id[file.path] = it->second;
        }

        // Copy implicit edges for commands that didn't have new deps discovered
        for (auto const& edge : old_index->edges()) {
            if (edge.type != pup::LinkType::Implicit)
                continue;

            // Skip if this command got new deps this build
            if (commands_with_new_deps.contains(edge.to))
                continue;

            // Find the source file in the old index
            auto const* old_file = old_index->find_file_by_id(edge.from);
            if (!old_file)
                continue;

            auto new_file_it = path_to_id.find(old_file->path);
            pup::NodeId new_from_id;
            if (new_file_it != path_to_id.end()) {
                new_from_id = new_file_it->second;
            } else {
                // File not in new index yet - add it
                new_from_id = next_id++;

                auto abs_path = resolve_path(old_file->path, root);
                auto content_hash = pup::Hash256 {};
                auto file_size = std::uint64_t { 0 };
                if (std::filesystem::exists(abs_path)) {
                    auto hash_result = pup::sha256_file(abs_path);
                    if (hash_result)
                        content_hash = *hash_result;

                    auto ec = std::error_code {};
                    file_size = std::filesystem::file_size(abs_path, ec);
                }

                // Compute parent directory chain and basename for v3 format
                auto fs_path = std::filesystem::path { old_file->path };
                auto parent_id = get_or_create_dir(fs_path.parent_path());
                auto basename = fs_path.filename().string();

                auto entry = pup::index::FileEntry {
                    .id = new_from_id,
                    .parent_id = parent_id,
                    .src_id = 0,
                    .type = pup::NodeType::File,
                    .flags = pup::NodeFlags::None,
                    .name = basename,
                    .path = old_file->path,
                    .size = file_size,
                    .mtime = get_file_mtime(abs_path),
                    .content_hash = content_hash,
                };
                index.add_file(std::move(entry));
                path_to_id[old_file->path] = new_from_id;
            }

            auto edge_key = std::pair { new_from_id, edge.to };
            if (added_edges.insert(edge_key).second) {
                index.add_edge(pup::index::EdgeEntry {
                    .from = new_from_id,
                    .to = edge.to,
                    .type = pup::LinkType::Implicit,
                    .group_cmd_id = 0,
                });
            }
        }
    }

    return index;
}

auto create_variant(
    std::filesystem::path const& root,
    std::string const& config_arg,
    std::optional<std::string> const& output_dir,
    bool verbose) -> pup::Result<void>
{
    auto config_path = std::filesystem::path { config_arg };
    auto abs_config = std::filesystem::path { root / config_path };

    if (!std::filesystem::exists(abs_config))
        return pup::make_error<void>(
            pup::ErrorCode::IoError,
            fmt::format("Config file not found: {}", config_arg));

    // Use custom output_dir if provided, else "build-{stem}"
    auto variant_name = std::string {};
    if (output_dir) {
        variant_name = *output_dir;

        if (variant_name.empty())
            return pup::make_error<void>(
                pup::ErrorCode::InvalidArgument,
                "Output directory cannot be empty");

        auto test_path = std::filesystem::path { variant_name };
        if (test_path.is_absolute())
            return pup::make_error<void>(
                pup::ErrorCode::InvalidArgument,
                "Output directory must be relative to project root");

        for (auto const& part : test_path) {
            if (part == "..")
                return pup::make_error<void>(
                    pup::ErrorCode::InvalidArgument,
                    "Output directory cannot contain '..' components");
        }
    } else {
        auto stem = std::string { config_path.stem().string() };
        if (stem.empty())
            return pup::make_error<void>(
                pup::ErrorCode::ParseError,
                fmt::format("Invalid config filename: {}", config_arg));
        variant_name = "build-" + stem;
    }
    auto variant_dir = std::filesystem::path { root / variant_name };

    if (!std::filesystem::exists(variant_dir)) {
        auto ec = std::error_code {};
        std::filesystem::create_directories(variant_dir, ec);
        if (ec)
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to create {}: {}", variant_name, ec.message()));
    }

    auto tup_config = std::filesystem::path { variant_dir / "tup.config" };

    if (std::filesystem::exists(tup_config)) {
        auto ec = std::error_code {};
        std::filesystem::remove(tup_config, ec);
    }

    auto display_target = std::filesystem::path {};

#ifdef _WIN32
    {
        auto ec = std::error_code {};
        std::filesystem::copy_file(abs_config, tup_config, ec);
        if (ec)
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to copy config: {}", ec.message()));
        display_target = abs_config;
    }
#else
    {
        auto ec = std::error_code {};
        auto rel_config = std::filesystem::relative(abs_config, variant_dir, ec);
        if (ec)
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to compute relative path: {}", ec.message()));

        std::filesystem::create_symlink(rel_config, tup_config, ec);
        if (ec)
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to create symlink: {}", ec.message()));
        display_target = rel_config;
    }
#endif

    fmt::print("Created variant '{}'\n", variant_name);
    if (verbose)
        fmt::print("  {} -> {}\n", tup_config.string(), display_target.string());

    return {};
}

auto cmd_init(Options const& /*opts*/) -> int
{
    auto root = std::filesystem::path { std::filesystem::current_path() };

    auto pup_dir = std::filesystem::path { root / PUP_DIR };
    if (std::filesystem::exists(pup_dir)) {
        fmt::print("Already initialized in \"{}\"\n", root.string());
        return EXIT_SUCCESS;
    }

    std::filesystem::create_directory(pup_dir);
    fmt::print("Initialized pup in \"{}\"\n", root.string());
    return EXIT_SUCCESS;
}

auto cmd_parse(Options const& opts) -> int
{
    auto graph_opts = BuildGraphOptions {
        .verbose = opts.verbose,
    };

    auto result = pup::Result<BuildGraphResult> { build_graph(opts, graph_opts) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    if (opts.verbose) {
        fmt::print("Project root: \"{}\"\n", ctx.layout.source_root.string());
        fmt::print("\nTupfiles:\n");
        for (auto const& dir : ctx.state.parsed) {
            auto tupfile_path = (dir == "." || dir.empty())
                ? ctx.layout.source_root / "Tupfile"
                : ctx.layout.source_root / dir / "Tupfile";
            fmt::print("  {}\n", tupfile_path.string());
        }
    }

    auto commands = ctx.graph.nodes_of_type(pup::NodeType::Command);

    if (opts.verbose && !commands.empty()) {
        fmt::print("\nCommands:\n");
        for (auto id : commands) {
            if (auto const* node = ctx.graph.get_node(id)) {
                auto display = node->display.empty() ? node->command : node->display;
                fmt::print("  {}\n", display);
            }
        }
    }

    fmt::print("Parsed {} Tupfile(s), {} commands\n", ctx.state.parsed.size(), commands.size());

    return EXIT_SUCCESS;
}

/// Escape string for DOT format (graphviz)
auto escape_dot_label(std::string_view s) -> std::string
{
    auto result = std::string {};
    result.reserve(s.size());
    for (auto c : s) {
        if (c == '"' || c == '\\')
            result += '\\';
        result += c;
    }
    return result;
}

auto cmd_export_script(Options const& opts) -> int
{
    auto graph_opts = BuildGraphOptions {
        .verbose = opts.verbose,
    };

    auto result = pup::Result<BuildGraphResult> { build_graph(opts, graph_opts) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto topo = pup::graph::topological_sort(ctx.graph);

    fmt::print("#!/bin/sh\n");
    fmt::print("# Generated by pup\n");
    fmt::print("set -e\n");
    fmt::print("cd \"{}\"\n\n", ctx.layout.source_root.string());

    // Collect all output directories that need to be created
    // Output files are Generated nodes that have a command as an input
    auto output_dirs = std::set<std::string> {};
    for (auto id : ctx.graph.all_nodes()) {
        auto const* node = ctx.graph.get_node(id);
        if (!node)
            continue;
        if (node->type != pup::NodeType::Generated && node->type != pup::NodeType::File)
            continue;

        auto node_path = ctx.graph.get_full_path(id);
        if (node_path.empty())
            continue;

        // Check if this is an output (has incoming edges from commands)
        auto inputs = ctx.graph.get_inputs(id);
        for (auto input_id : inputs) {
            auto const* input = ctx.graph.get_node(input_id);
            if (input && input->type == pup::NodeType::Command) {
                auto path = std::filesystem::path { node_path };
                if (path.has_parent_path()) {
                    auto parent = path.parent_path().string();
                    if (!parent.empty() && parent != ".")
                        output_dirs.insert(parent);
                }
                break;
            }
        }
    }

    // Print mkdir commands for output directories
    if (!output_dirs.empty()) {
        fmt::print("# Create output directories\n");
        for (auto const& dir : output_dirs)
            fmt::print("mkdir -p \"{}\"\n", dir);
        fmt::print("\n");
    }

    // Print build commands
    for (auto id : topo.order) {
        auto const* node = ctx.graph.get_node(id);
        if (!node || node->type != pup::NodeType::Command)
            continue;

        auto dir = node->source_dir.empty() ? std::string { "." } : node->source_dir;
        fmt::print("(cd \"{}\" && {})\n", dir, node->command);
    }

    return EXIT_SUCCESS;
}

auto cmd_export_graph(Options const& opts) -> int
{
    auto graph_opts = BuildGraphOptions {
        .verbose = opts.verbose,
    };

    auto result = pup::Result<BuildGraphResult> { build_graph(opts, graph_opts) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    if (opts.summary) {
        // Human-readable summary (old cmd_graph behavior)
        auto commands = ctx.graph.nodes_of_type(pup::NodeType::Command);
        fmt::print("Tupfiles: {}\n", ctx.state.parsed.size());
        fmt::print("Nodes: {}\n", ctx.graph.node_count());
        fmt::print("Edges: {}\n", ctx.graph.edge_count());
        fmt::print("Commands: {}\n", commands.size());

        if (opts.verbose) {
            fmt::print("\nCommands:\n");
            for (auto id : commands) {
                if (auto const* node = ctx.graph.get_node(id)) {
                    auto display = node->display.empty() ? node->command : node->display;
                    fmt::print("  {}\n", display);
                }
            }
        }
        return EXIT_SUCCESS;
    }

    // DOT format output
    fmt::print("digraph G {{\n");
    fmt::print("  rankdir=LR;\n");

    for (auto id : ctx.graph.all_nodes()) {
        auto const* node = ctx.graph.get_node(id);
        if (!node)
            continue;

        auto label = escape_dot_label(
            node->type == pup::NodeType::Command
                ? (node->display.empty() ? node->command : node->display)
                : ctx.graph.get_full_path(id));

        fmt::print("  n{} [label=\"{}\"];\n", id, label);

        for (auto input_id : ctx.graph.get_inputs(id))
            fmt::print("  n{} -> n{};\n", input_id, id);
    }

    fmt::print("}}\n");
    return EXIT_SUCCESS;
}

struct CleanContext {
    std::filesystem::path root;
    std::filesystem::path build_dir;
    bool is_in_tree;
};

auto resolve_clean_context(Options const& opts) -> std::optional<CleanContext>
{
    auto root = pup::find_project_root(std::filesystem::current_path());
    if (!root)
        return std::nullopt;

    auto build_dir = std::filesystem::path {};
    auto is_in_tree = false;

    if (!opts.build_dir.empty()) {
        // Explicit -B specified
        build_dir = std::filesystem::path { opts.build_dir };
        if (build_dir.is_relative())
            build_dir = *root / build_dir;
        is_in_tree = (build_dir == *root);
    } else if (std::filesystem::exists(*root / "tup.config")) {
        // In-tree build: tup.config at project root
        build_dir = *root;
        is_in_tree = true;
    } else if (std::filesystem::exists(*root / ".pup")) {
        // In-tree build: .pup at project root
        build_dir = *root;
        is_in_tree = true;
    } else if (auto detected = pup::find_variant_dir(*root)) {
        // Out-of-tree variant directory
        build_dir = *root / *detected;
        is_in_tree = false;
    } else {
        return std::nullopt;
    }

    return CleanContext { *root, build_dir, is_in_tree };
}

struct CleanResult {
    std::size_t removed_count = 0;
    std::size_t error_count = 0;
    std::set<std::filesystem::path> output_dirs = {};
};

auto remove_indexed_outputs(
    std::filesystem::path const& index_path,
    bool dry_run,
    bool verbose) -> CleanResult
{
    auto result = CleanResult {};

    auto reader_result = pup::index::IndexReader::open(index_path);
    if (!reader_result)
        return result;

    auto index_result = reader_result->read();
    if (!index_result)
        return result;

    auto const& index = *index_result;

    for (auto const& file : index.files()) {
        if (file.type != pup::NodeType::Generated)
            continue;

        auto path = std::filesystem::path { file.path };
        // Add all parent directories
        for (auto parent = path.parent_path();
             !parent.empty() && parent != parent.parent_path();
             parent = parent.parent_path())
            result.output_dirs.insert(parent);

        if (!std::filesystem::exists(path))
            continue;

        if (dry_run) {
            fmt::print("Would remove: {}\n", file.path);
            ++result.removed_count;
            continue;
        }

        auto ec = std::error_code {};
        if (std::filesystem::remove(path, ec)) {
            ++result.removed_count;
            if (verbose)
                fmt::print("Removed: {}\n", file.path);
        } else if (ec) {
            fmt::print(stderr, "Error removing {}: {}\n", file.path, ec.message());
            ++result.error_count;
        }
    }

    return result;
}

auto remove_empty_output_directories(
    std::set<std::filesystem::path> const& output_dirs,
    std::filesystem::path const& build_dir,
    std::filesystem::path const& source_dir,
    bool dry_run,
    bool verbose) -> std::size_t
{
    auto removed = std::size_t { 0 };

    // Sort by depth (deepest first) so children are removed before parents
    auto dirs = std::vector<std::filesystem::path>(output_dirs.begin(), output_dirs.end());
    std::sort(dirs.begin(), dirs.end(), [](auto const& a, auto const& b) {
        return a.string().size() > b.string().size();
    });

    for (auto const& dir : dirs) {
        // Never remove source directory
        if (dir == source_dir)
            continue;

        // Must be within or equal to build_dir
        auto rel = std::filesystem::relative(dir, build_dir);
        if (rel.string().starts_with(".."))
            continue;

        // Must be empty
        if (!std::filesystem::exists(dir) || !std::filesystem::is_empty(dir))
            continue;

        if (dry_run) {
            fmt::print("Would remove empty dir: {}\n", dir.string());
        } else {
            std::filesystem::remove(dir);
            ++removed;
            if (verbose)
                fmt::print("Removed empty dir: {}\n", dir.string());
        }
    }
    return removed;
}

auto cmd_clean(Options const& opts) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        fmt::print(stderr, "Error: No build directory found (use -B to specify)\n");
        return EXIT_FAILURE;
    }

    auto index_path = ctx->build_dir / ".pup" / "index";
    if (!std::filesystem::exists(index_path)) {
        fmt::print("Nothing to clean (no index found)\n");
        return EXIT_SUCCESS;
    }

    auto result = remove_indexed_outputs(index_path, opts.dry_run, opts.verbose);

    // Remove empty directories that contained outputs
    auto dirs_removed = remove_empty_output_directories(
        result.output_dirs, ctx->build_dir, ctx->root, opts.dry_run, opts.verbose);

    if (opts.dry_run)
        fmt::print("Would remove {} files, {} directories\n", result.removed_count, dirs_removed);
    else
        fmt::print("Removed {} files, {} directories\n", result.removed_count, dirs_removed);

    return result.error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

auto cmd_distclean(Options const& opts) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        fmt::print(stderr, "Error: No build directory found (use -B to specify)\n");
        return EXIT_FAILURE;
    }

    auto index_path = ctx->build_dir / ".pup" / "index";
    auto error_count = std::size_t { 0 };
    auto output_dirs = std::set<std::filesystem::path> {};

    // Remove indexed outputs (if index exists)
    if (std::filesystem::exists(index_path)) {
        auto result = remove_indexed_outputs(index_path, opts.dry_run, opts.verbose);
        error_count += result.error_count;
        output_dirs = std::move(result.output_dirs);
    }

    // Remove .pup directory
    auto pup_dir = ctx->build_dir / ".pup";
    if (std::filesystem::exists(pup_dir)) {
        if (opts.dry_run) {
            fmt::print("Would remove: {}\n", pup_dir.string());
        } else {
            if (opts.verbose)
                fmt::print("Removing: {}\n", pup_dir.string());
            std::filesystem::remove_all(pup_dir);
        }
    }

    // Remove tup.config
    auto config_path = ctx->build_dir / "tup.config";
    if (std::filesystem::exists(config_path)) {
        if (opts.dry_run) {
            fmt::print("Would remove: {}\n", config_path.string());
        } else {
            if (opts.verbose)
                fmt::print("Removing: {}\n", config_path.string());
            std::filesystem::remove(config_path);
        }
    }

    // Remove empty directories that contained outputs (including build_dir if not source_dir)
    output_dirs.insert(ctx->build_dir);
    remove_empty_output_directories(output_dirs, ctx->build_dir, ctx->root, opts.dry_run, opts.verbose);

    if (!opts.dry_run)
        fmt::print("Project reset complete\n");

    return error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

auto cmd_variant(Options const& opts) -> int
{
    if (opts.targets.empty()) {
        fmt::print(stderr, "Error: No config file specified\n");
        fmt::print(stderr, "Usage: pup variant <config> [output_dir]\n");
        return EXIT_FAILURE;
    }

    auto root = std::optional<std::filesystem::path> { pup::find_project_root(std::filesystem::current_path()) };
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project\n");
        fmt::print(stderr, "Run 'pup init' first\n");
        return EXIT_FAILURE;
    }

    // First argument is config file, optional second is output directory
    auto config_path = opts.targets[0];
    auto output_dir = std::optional<std::string> {};
    if (opts.targets.size() > 1)
        output_dir = opts.targets[1];

    auto result = pup::Result<void> { create_variant(*root, config_path, output_dir, opts.verbose) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

// Forward declaration
auto cmd_export_compdb(Options const& opts) -> int;

auto cmd_export(Options const& opts) -> int
{
    if (opts.export_format.empty()) {
        fmt::print(stderr, "Usage: pup export <format>\n");
        fmt::print(stderr, "Formats: script, compdb, graph\n");
        return EXIT_FAILURE;
    }

    if (opts.export_format == "script")
        return cmd_export_script(opts);
    if (opts.export_format == "compdb")
        return cmd_export_compdb(opts);
    if (opts.export_format == "graph")
        return cmd_export_graph(opts);

    fmt::print(stderr, "Unknown export format: {}\n", opts.export_format);
    fmt::print(stderr, "Formats: script, compdb, graph\n");
    return EXIT_FAILURE;
}

auto cmd_export_compdb(Options const& opts) -> int
{
    auto graph_opts = BuildGraphOptions {
        .verbose = opts.verbose,
    };

    auto result = pup::Result<BuildGraphResult> { build_graph(opts, graph_opts) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Helper: escape string for JSON (RFC 8259)
    auto escape_json = [](std::string_view s) -> std::string {
        auto result = std::string {};
        result.reserve(s.size());
        for (auto c : s) {
            switch (c) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    continue; // Skip other control characters
                result += c;
            }
        }
        return result;
    };

    // Output JSON
    fmt::print("[\n");
    auto commands = ctx.graph.nodes_of_type(pup::NodeType::Command);
    auto first = true;

    for (auto id : commands) {
        auto const* node = ctx.graph.get_node(id);
        if (!node)
            continue;

        // Find source file from inputs (required by compile_commands.json spec)
        auto source_file = std::string {};
        for (auto input_id : ctx.graph.get_inputs(id)) {
            auto input_path = ctx.graph.get_full_path(input_id);
            if (input_path.empty())
                continue;
            if (input_path.ends_with(".c") || input_path.ends_with(".cc") || input_path.ends_with(".cpp") || input_path.ends_with(".cxx") || input_path.ends_with(".C") || input_path.ends_with(".S") || input_path.ends_with(".s")) {
                source_file = std::move(input_path);
                break;
            }
        }

        // Find output file
        auto output_file = std::string {};
        for (auto output_id : ctx.graph.get_outputs(id)) {
            auto output_path = ctx.graph.get_full_path(output_id);
            if (output_path.empty())
                continue;
            if (output_path.ends_with(".o") || output_path.ends_with(".obj")) {
                output_file = std::move(output_path);
                break;
            }
        }

        if (source_file.empty())
            continue;

        // Compute working directory
        auto working_dir = ctx.layout.source_root;
        if (!node->source_dir.empty())
            working_dir /= node->source_dir;

        auto args = pup::core::tokenize_shell_command(node->command);
        if (args.empty())
            continue;

        if (!first)
            fmt::print(",\n");
        first = false;

        fmt::print("  {{\n");
        fmt::print("    \"directory\": \"{}\",\n", escape_json(working_dir.string()));

        fmt::print("    \"arguments\": [");
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0)
                fmt::print(", ");
            fmt::print("\"{}\"", escape_json(args[i]));
        }
        fmt::print("],\n");

        fmt::print("    \"file\": \"{}\"", escape_json(source_file));
        if (!output_file.empty())
            fmt::print(",\n    \"output\": \"{}\"", escape_json(output_file));
        fmt::print("\n  }}");
    }

    fmt::print("\n]\n");
    return EXIT_SUCCESS;
}

auto cmd_build(Options const& opts) -> int
{
    // Implicit dependency tracking is enabled by default (set PUP_IMPLICIT_DEPS=0 to disable)
    auto pattern_registry = std::optional<pup::graph::RulePatternRegistry> {};
    auto implicit_deps_disabled = false;
    if (auto const* env = std::getenv("PUP_IMPLICIT_DEPS"); env && std::string_view { env } == "0")
        implicit_deps_disabled = true;

    if (!implicit_deps_disabled) {
        pattern_registry.emplace();
        pattern_registry->register_pattern(pup::graph::make_gcc_depfile_pattern());
        if (opts.verbose)
            fmt::print("Implicit dependency tracking enabled\n");
    }

    auto graph_opts = BuildGraphOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = true,
        .pattern_registry = pattern_registry ? &*pattern_registry : nullptr,
    };

    auto result = pup::Result<BuildGraphResult> { build_graph(opts, graph_opts) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto num_commands = std::size_t { ctx.graph.nodes_of_type(pup::NodeType::Command).size() };

    if (num_commands == 0) {
        fmt::print("Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    auto index_path = ctx.layout.index_path();
    auto old_index = std::optional<pup::index::Index> {};
    auto use_incremental = false;
    auto changed_files = std::vector<std::string> {};

    if (std::filesystem::exists(index_path)) {
        auto reader_result = pup::Result<pup::index::IndexReader> { pup::index::IndexReader::open(index_path) };
        if (reader_result) {
            auto index_result = pup::Result<pup::index::Index> { reader_result->read() };
            if (index_result) {
                old_index = std::move(*index_result);
                changed_files = find_changed_files_with_implicit(ctx.layout.source_root, *old_index, opts.verbose);
                changed_files = expand_implicit_deps(changed_files, *old_index, ctx.graph);

                if (changed_files.empty()) {
                    fmt::print("Nothing to do (up to date).\n");
                    return EXIT_SUCCESS;
                }

                use_incremental = true;
                if (opts.verbose)
                    fmt::print("Incremental build: {} changed files\n", changed_files.size());
            }
        }
    }

    auto sched_opts = pup::exec::SchedulerOptions {
        .jobs = opts.jobs,
        .keep_going = opts.keep_going,
        .dry_run = opts.dry_run,
        .verbose = opts.verbose,
        .source_root = ctx.layout.source_root,
        .output_root = ctx.layout.output_root,
        .variant_dir = ctx.layout.variant_dir,
    };

    auto scheduler = pup::exec::Scheduler { sched_opts };
    auto discovered_deps = std::unordered_map<pup::NodeId, std::vector<std::string>> {};
    auto deps_mutex = std::mutex {};

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run)
            fmt::print("{}\n", job.display);
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& result) {
        if (!result.success) {
            fmt::print(stderr, "FAILED: {}\n", job.display);
            if (!result.output.empty())
                fmt::print(stderr, "{}\n", result.output);
        } else if (!result.discovered_deps.empty()) {
            auto lock = std::lock_guard { deps_mutex };
            // If deps_for_command is set, the deps belong to that command (for generated rules)
            auto target_id = result.deps_for_command != pup::INVALID_NODE_ID
                ? result.deps_for_command
                : job.id;
            auto& deps = discovered_deps[target_id];

            // Resolve paths relative to job's working directory and normalize to source root
            for (auto const& dep_path : result.discovered_deps) {
                try {
                    auto resolved = std::filesystem::path {};
                    if (std::filesystem::path { dep_path }.is_absolute()) {
                        resolved = std::filesystem::weakly_canonical(dep_path);
                    } else {
                        // dep_path is relative to job.working_dir
                        resolved = std::filesystem::weakly_canonical(job.working_dir / dep_path);
                    }

                    // Make relative to source root if under it
                    if (is_path_under_root(resolved, ctx.layout.source_root)) {
                        deps.push_back(std::filesystem::relative(resolved, ctx.layout.source_root).string());
                    } else {
                        deps.push_back(resolved.string());
                    }
                } catch (std::filesystem::filesystem_error const& e) {
                    if (opts.verbose)
                        fmt::print(stderr, "Warning: Skipping dependency '{}': {}\n", dep_path, e.what());
                }
            }
        }
    });

    auto completed = std::size_t { 0 };
    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        completed = done;
        if (!opts.verbose) {
            fmt::print("\r[{}/{}] ", done, total);
            std::fflush(stdout);
        }
    });

    auto start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto build_result = pup::Result<pup::exec::BuildStats> {};
    if (use_incremental && old_index) {
        build_result = scheduler.build_incremental(ctx.graph, *old_index, changed_files);
    } else {
        build_result = scheduler.build(ctx.graph);
    }
    auto end = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto duration = std::chrono::milliseconds { std::chrono::duration_cast<std::chrono::milliseconds>(end - start) };

    if (!opts.verbose)
        fmt::print("\n");

    if (!build_result) {
        fmt::print(stderr, "Build failed: {}\n", build_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0)
        fmt::print("Build completed: {} commands ({} failed) in {}ms\n",
            stats.completed_jobs, stats.failed_jobs, duration.count());
    else
        fmt::print("Build completed: {} commands in {}ms\n",
            stats.completed_jobs, duration.count());

    if (stats.failed_jobs == 0 && !opts.dry_run) {
        auto const* old_index_ptr = old_index ? &*old_index : nullptr;
        auto index = pup::index::Index { build_index(ctx.graph, discovered_deps, ctx.layout.source_root, old_index_ptr) };
        auto writer = pup::index::IndexWriter {};
        auto write_result = pup::Result<void> { writer.write(index_path, index) };
        if (!write_result) {
            fmt::print(stderr, "Warning: Failed to save index: {}\n", write_result.error().message);
        } else if (opts.verbose) {
            fmt::print("Saved index: {} files, {} commands, {} edges\n",
                index.file_count(), index.command_count(), index.edge_count());
        }
    }

    return stats.failed_jobs > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // anonymous namespace

auto main(int argc, char** argv) -> int
{
    auto opts = Options { parse_args(argc, argv) };

    if (opts.help) {
        print_usage();
        return EXIT_SUCCESS;
    }

    if (opts.version) {
        print_version();
        return EXIT_SUCCESS;
    }

    if (opts.command == "init")
        return cmd_init(opts);
    if (opts.command == "parse")
        return cmd_parse(opts);
    if (opts.command == "export")
        return cmd_export(opts);
    if (opts.command == "build")
        return cmd_build(opts);
    if (opts.command == "clean")
        return cmd_clean(opts);
    if (opts.command == "distclean")
        return cmd_distclean(opts);
    if (opts.command == "variant")
        return cmd_variant(opts);

    fmt::print(stderr, "Unknown command: {}\n", opts.command);
    print_usage();
    return EXIT_FAILURE;
}
