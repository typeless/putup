// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/core/hash.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/result.hpp"
#include "pup/core/types.hpp"
#include "pup/exec/runner.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/builder.hpp"
#include "pup/graph/dag.hpp"
#include "pup/index/reader.hpp"
#include "pup/index/writer.hpp"
#include "pup/parser/config.hpp"
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
    std::string command = {};
    std::string variant = {};
    std::string source_dir = {}; ///< -S: source directory
    std::string build_dir = {};  ///< -B: build/output directory
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
               "  graph             Print dependency graph\n"
               "  clean             Remove generated files\n"
               "  variant <config> [dir]  Create variant build directory\n"
               "\nOptions:\n"
               "  -j, --jobs N       Run N jobs in parallel\n"
               "  -k, --keep-going   Continue after failures\n"
               "  -n, --dry-run      Print commands without executing\n"
               "  -v, --verbose      Verbose output\n"
               "  -S DIR             Source directory (default: auto-detect)\n"
               "  -B DIR             Build/output directory (default: source)\n"
               "  --variant=DIR      Use DIR as variant subdirectory\n"
               "  --version          Print version\n"
               "  -h, --help         Print this help\n"
               "\nEnvironment:\n"
               "  PUP_SOURCE_DIR     Source directory (overridden by -S)\n"
               "  PUP_BUILD_DIR      Build directory (overridden by -B)\n");
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
                opts.jobs = static_cast<std::size_t>(std::stoi(argv[++i]));
            }
        } else if (arg.starts_with("-j")) {
            opts.jobs = static_cast<std::size_t>(std::stoi(std::string { arg.substr(2) }));
        } else if (arg.starts_with("--variant=")) {
            opts.variant = std::string { arg.substr(10) };
        } else if (arg == "-S") {
            if (i + 1 < argc)
                opts.source_dir = std::string { argv[++i] };
        } else if (arg == "-B") {
            if (i + 1 < argc)
                opts.build_dir = std::string { argv[++i] };
        } else if (!arg.starts_with("-")) {
            if (opts.command.empty())
                opts.command = std::string { arg };
            else
                opts.targets.emplace_back(arg);
        }
    }

    if (opts.command.empty())
        opts.command = "build";

    return opts;
}

auto compute_variantdir(
    std::filesystem::path const& source_dir,
    std::filesystem::path const& variant_dir) -> std::string
{
    if (variant_dir.empty())
        return ".";

    auto output_dir = std::filesystem::path { variant_dir / source_dir };
    auto rel = std::filesystem::relative(output_dir, source_dir);
    return rel.string();
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
auto discover_tupfile_dirs(std::filesystem::path const& root)
    -> std::set<std::filesystem::path>
{
    auto dirs = std::set<std::filesystem::path> {};
    auto ec = std::error_code {};

    for (auto const& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;
        if (entry.path().filename() != "Tupfile")
            continue;

        auto dir = std::filesystem::path { entry.path().parent_path() };

        // Skip variant directories (contain tup.config)
        if (std::filesystem::exists(dir / "tup.config"))
            continue;

        // Store relative path from root
        auto rel = std::filesystem::relative(dir, root);
        dirs.insert(rel.empty() || rel == "." ? std::filesystem::path { "." } : rel);
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
        variant_dir);

    // Create recursive callback for demand-driven parsing
    // Pass base_vars (not the local vars copy) so each directory starts fresh
    auto request_directory = [&](std::filesystem::path const& dir) -> pup::Result<void> {
        return parse_directory(dir, state, builder, graph, root, base_vars, config_vars, variant_dir, verbose);
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

auto find_changed_files_with_implicit(
    std::filesystem::path const& root,
    pup::index::Index const& old_index) -> std::vector<std::string>
{
    auto changed = std::vector<std::string> {};

    for (auto const& file : old_index.files()) {
        if (file.type != pup::NodeType::File && file.type != pup::NodeType::Generated)
            continue;

        auto path = resolve_path(file.path, root);

        struct stat st = {};
        if (::stat(path.c_str(), &st) < 0) {
            changed.push_back(file.path);
            continue;
        }

        auto current_mtime = pup::FileTime {
            .seconds = st.st_mtim.tv_sec,
            .nanoseconds = static_cast<std::int32_t>(st.st_mtim.tv_nsec),
        };

        if (current_mtime != file.mtime) {
            changed.push_back(file.path);
            continue;
        }

        auto current_size = static_cast<std::uint64_t>(st.st_size);
        if (current_size != file.size) {
            changed.push_back(file.path);
            continue;
        }

        if (file.content_hash != pup::ZERO_HASH) {
            auto hash_result = pup::sha256_file(path);
            if (!hash_result || *hash_result != file.content_hash)
                changed.push_back(file.path);
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
            if (edge.type != pup::LinkType::Implicit)
                continue;
            if (edge.from != file_id)
                continue;

            auto cmd_id = pup::NodeId { edge.to };
            auto const* cmd = index.find_command_by_id(cmd_id);
            if (!cmd)
                continue;

            auto cmd_node_id = graph.find_by_command(cmd->command);
            if (!cmd_node_id)
                continue;

            for (auto input_id : graph.get_inputs(*cmd_node_id)) {
                auto const* input_node = graph.get_node(input_id);
                if (input_node && !input_node->path.empty()) {
                    if (added.insert(input_node->path).second)
                        result.push_back(input_node->path);
                }
            }
        }
    }

    return result;
}

auto build_index(
    pup::graph::BuildGraph const& graph,
    std::unordered_map<pup::NodeId, std::vector<std::string>> const& discovered_deps,
    std::filesystem::path const& root) -> pup::index::Index
{
    auto index = pup::index::Index {};
    auto path_to_id = std::unordered_map<std::string, pup::NodeId> {};

    auto max_id = pup::NodeId { 0 };
    for (auto const& node : graph) {
        if (node.id > max_id)
            max_id = node.id;

        if (node.type == pup::NodeType::File || node.type == pup::NodeType::Generated) {
            auto file_path = std::filesystem::path { root / node.path };
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
                .path = node.path,
                .size = file_size,
                .mtime = get_file_mtime(file_path),
                .content_hash = content_hash,
            };
            index.add_file(std::move(entry));
            path_to_id[node.path] = node.id;
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

                auto entry = pup::index::FileEntry {
                    .id = dep_id,
                    .parent_id = 0,
                    .src_id = 0,
                    .type = pup::NodeType::File,
                    .flags = pup::NodeFlags::None,
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
        std::filesystem::create_directory(variant_dir, ec);
        if (ec)
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to create {}: {}", variant_name, ec.message()));
    }

    auto tup_config = std::filesystem::path { variant_dir / "tup.config" };
    auto rel_config = std::filesystem::path { "../" + config_path.string() };

    if (std::filesystem::exists(tup_config)) {
        auto ec = std::error_code {};
        std::filesystem::remove(tup_config, ec);
    }

#ifdef _WIN32
    auto ec = std::error_code {};
    std::filesystem::copy_file(abs_config, tup_config, ec);
    if (ec)
        return pup::make_error<void>(
            pup::ErrorCode::IoError,
            fmt::format("Failed to copy config: {}", ec.message()));
#else
    auto ec = std::error_code {};
    std::filesystem::create_symlink(rel_config, tup_config, ec);
    if (ec)
        return pup::make_error<void>(
            pup::ErrorCode::IoError,
            fmt::format("Failed to create symlink: {}", ec.message()));
#endif

    fmt::print("Created variant '{}'\n", variant_name);
    if (verbose)
        fmt::print("  {} -> {}\n", tup_config.string(), rel_config.string());

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
    auto root = pup::find_project_root(std::filesystem::current_path());
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project (no Tupfile.ini, Tupfile, or .pup/ found)\n");
        return EXIT_FAILURE;
    }

    if (opts.verbose)
        fmt::print("Project root: \"{}\"\n", root->string());

    // Discover all Tupfiles
    auto tupfile_dirs = std::set<std::filesystem::path> { discover_tupfile_dirs(*root) };
    if (tupfile_dirs.empty()) {
        fmt::print(stderr, "Error: No Tupfiles found in project\n");
        return EXIT_FAILURE;
    }

    auto total_statements = std::size_t { 0 };
    auto total_rules = std::size_t { 0 };
    auto total_macros = std::size_t { 0 };
    auto total_assignments = std::size_t { 0 };

    for (auto const& dir : tupfile_dirs) {
        auto tupfile_path = std::filesystem::path {
            dir == "." ? *root / "Tupfile" : *root / dir / "Tupfile"
        };

        auto source = std::optional<std::string> { read_file(tupfile_path) };
        if (!source) {
            fmt::print(stderr, "Error: Failed to read Tupfile at \"{}\"\n", tupfile_path.string());
            continue;
        }

        auto parser = pup::parser::Parser { *source, tupfile_path.string() };
        auto result = pup::Result<pup::parser::Tupfile> { parser.parse() };

        if (!result) {
            fmt::print(stderr, "Parse error in {}: {}\n", tupfile_path.string(), result.error().message);
            return EXIT_FAILURE;
        }

        auto const& tupfile = *result;
        total_statements += tupfile.statements.size();

        if (opts.verbose) {
            fmt::print("{}:\n", tupfile_path.string());
            for (auto const& stmt : tupfile.statements) {
                if (auto const* rule = stmt->as<pup::parser::Rule>()) {
                    fmt::print("  Rule: {} inputs -> {} outputs\n",
                        rule->inputs.size(), rule->outputs.size());
                    ++total_rules;
                } else if (auto const* assign = stmt->as<pup::parser::Assignment>()) {
                    fmt::print("  Assignment: {}\n", assign->name);
                    ++total_assignments;
                } else if (auto const* macro = stmt->as<pup::parser::BangMacro>()) {
                    fmt::print("  Macro: !{}\n", macro->name);
                    ++total_macros;
                }
            }
        }
    }

    fmt::print("Parsed {} statements from {} Tupfile(s)\n", total_statements, tupfile_dirs.size());

    return EXIT_SUCCESS;
}

auto cmd_graph(Options const& opts) -> int
{
    auto root = std::optional<std::filesystem::path> { pup::find_project_root(std::filesystem::current_path()) };
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project (no Tupfile.ini, Tupfile, or .pup/ found)\n");
        return EXIT_FAILURE;
    }

    // Discover all Tupfiles
    auto state = TupfileParseState {};
    state.available = discover_tupfile_dirs(*root);

    if (state.available.empty()) {
        fmt::print(stderr, "Error: No Tupfiles found in project\n");
        return EXIT_FAILURE;
    }

    auto vars = pup::parser::VarDb {};
    auto config_vars = pup::parser::VarDb {};
    auto variant_dir = std::filesystem::path {};

    auto builder_opts = pup::graph::BuilderOptions {
        .source_root = *root,
        .output_root = *root,
        .variant_dir = variant_dir,
        .expand_globs = true,
    };

    auto builder = pup::graph::GraphBuilder { builder_opts };
    auto graph = pup::graph::BuildGraph {};

    // Start with root Tupfile if it exists
    auto root_rel = std::filesystem::path { "." };
    if (state.available.contains(root_rel)) {
        if (auto result = parse_directory(root_rel, state, builder, graph, *root, vars, config_vars, variant_dir, opts.verbose); !result) {
            fmt::print(stderr, "Error: {}\n", result.error().message);
            return EXIT_FAILURE;
        }
    }

    // Parse remaining directories
    for (auto const& dir : state.available) {
        if (state.parsed.contains(dir))
            continue;
        if (auto result = parse_directory(dir, state, builder, graph, *root, vars, config_vars, variant_dir, opts.verbose); !result) {
            fmt::print(stderr, "Error: {}\n", result.error().message);
            return EXIT_FAILURE;
        }
    }

    auto num_nodes = std::size_t { graph.node_count() };
    auto num_edges = std::size_t { graph.edge_count() };
    auto commands = std::vector<pup::NodeId> { graph.nodes_of_type(pup::NodeType::Command) };
    fmt::print("Tupfiles: {}\n", state.parsed.size());
    fmt::print("Nodes: {}\n", num_nodes);
    fmt::print("Edges: {}\n", num_edges);
    fmt::print("Commands: {}\n", commands.size());

    if (opts.verbose) {
        fmt::print("\nCommands:\n");
        for (auto id : commands) {
            if (auto const* node = graph.get_node(id)) {
                auto display = std::string { node->display.empty() ? node->command : node->display };
                fmt::print("  {}\n", display);
            }
        }
    }

    return EXIT_SUCCESS;
}

auto cmd_clean(Options const& opts) -> int
{
    auto root = std::optional<std::filesystem::path> { pup::find_project_root(std::filesystem::current_path()) };
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project (no Tupfile.ini, Tupfile, or .pup/ found)\n");
        return EXIT_FAILURE;
    }

    // Find variant directory
    auto variant_dir = std::optional<std::filesystem::path> {};
    if (!opts.variant.empty()) {
        variant_dir = *root / opts.variant;
    } else if (auto var_name = pup::find_variant_dir(*root)) {
        variant_dir = *root / *var_name;
    }

    if (!variant_dir || !std::filesystem::exists(*variant_dir)) {
        fmt::print(stderr, "Error: No variant directory found (nothing to clean)\n");
        return EXIT_FAILURE;
    }

    auto removed_count = std::size_t { 0 };
    auto error_count = std::size_t { 0 };

    // Remove all files in variant directory except tup.config
    for (auto const& entry : std::filesystem::directory_iterator(*variant_dir)) {
        auto filename = std::string { entry.path().filename().string() };

        // Preserve tup.config and .gitignore
        if (filename == "tup.config" || filename == ".gitignore")
            continue;

        if (opts.dry_run) {
            fmt::print("Would remove: {}\n", entry.path().string());
            ++removed_count;
            continue;
        }

        auto ec = std::error_code {};
        if (entry.is_directory()) {
            auto count = std::filesystem::remove_all(entry.path(), ec);
            if (ec) {
                fmt::print(stderr, "Error removing {}: {}\n", entry.path().string(), ec.message());
                ++error_count;
            } else {
                removed_count += count;
                if (opts.verbose)
                    fmt::print("Removed: {} ({} files)\n", entry.path().string(), count);
            }
        } else {
            if (std::filesystem::remove(entry.path(), ec)) {
                ++removed_count;
                if (opts.verbose)
                    fmt::print("Removed: {}\n", entry.path().string());
            } else if (ec) {
                fmt::print(stderr, "Error removing {}: {}\n", entry.path().string(), ec.message());
                ++error_count;
            }
        }
    }

    if (opts.dry_run)
        fmt::print("Would remove {} items\n", removed_count);
    else
        fmt::print("Removed {} items\n", removed_count);

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

auto cmd_build(Options const& opts) -> int
{
    // Discover project layout (source/output directories)
    auto layout_opts = pup::LayoutOptions {};
    if (!opts.source_dir.empty())
        layout_opts.source_dir = std::filesystem::path { opts.source_dir };
    if (!opts.build_dir.empty())
        layout_opts.build_dir = std::filesystem::path { opts.build_dir };

    auto layout_result = pup::Result<pup::ProjectLayout> { pup::discover_layout(layout_opts) };
    if (!layout_result) {
        fmt::print(stderr, "Error: {}\n", layout_result.error().message);
        return EXIT_FAILURE;
    }
    auto layout = pup::ProjectLayout { std::move(*layout_result) };

    // Override variant_dir from --variant if specified
    if (!opts.variant.empty())
        layout.variant_dir = std::filesystem::path { opts.variant };

    // Auto-initialize if Tupfile.ini exists but .pup/ doesn't
    auto pup_dir = layout.pup_dir();
    if (!std::filesystem::exists(pup_dir)) {
        if (std::filesystem::exists(layout.source_root / "Tupfile.ini")) {
            std::filesystem::create_directories(pup_dir);
            fmt::print("Initialized pup in \"{}\"\n", pup_dir.string());
        }
    }

    // Discover all directories containing Tupfiles
    auto state = TupfileParseState {};
    state.available = discover_tupfile_dirs(layout.source_root);

    if (state.available.empty()) {
        fmt::print(stderr, "Error: No Tupfiles found in project\n");
        return EXIT_FAILURE;
    }

    if (opts.verbose)
        fmt::print("Found {} directories with Tupfiles\n", state.available.size());

    auto variant_dir = layout.variant_dir;

    // Load config variables from tup.config
    auto config_vars = pup::parser::VarDb {};
    if (!variant_dir.empty()) {
        auto config_path = std::filesystem::path { layout.output_root / variant_dir / "tup.config" };
        if (std::filesystem::exists(config_path)) {
            auto config_result = pup::Result<pup::parser::VarDb> { pup::parser::parse_config(config_path) };
            if (config_result) {
                config_vars = std::move(*config_result);
                if (opts.verbose)
                    fmt::print("Loaded config from {}\n", config_path.string());
            }
        }
    }

    auto vars = pup::parser::VarDb {};

    auto builder_opts = pup::graph::BuilderOptions {
        .source_root = layout.source_root,
        .output_root = layout.output_root,
        .variant_dir = variant_dir,
        .expand_globs = true,
    };

    auto builder = pup::graph::GraphBuilder { builder_opts };
    auto graph = pup::graph::BuildGraph {};

    // Start with root Tupfile if it exists
    auto root_rel = std::filesystem::path { "." };
    if (state.available.contains(root_rel)) {
        auto result = pup::Result<void> {
            parse_directory(root_rel, state, builder, graph, layout.source_root, vars, config_vars, variant_dir, opts.verbose)
        };
        if (!result && !opts.keep_going) {
            fmt::print(stderr, "Error: {}\n", result.error().message);
            return EXIT_FAILURE;
        }
    }

    // Parse any remaining unparsed directories (orphan Tupfiles)
    for (auto const& dir : state.available) {
        if (state.parsed.contains(dir))
            continue;
        auto result = pup::Result<void> {
            parse_directory(dir, state, builder, graph, layout.source_root, vars, config_vars, variant_dir, opts.verbose)
        };
        if (!result && !opts.keep_going) {
            fmt::print(stderr, "Error: {}\n", result.error().message);
            return EXIT_FAILURE;
        }
    }

    if (opts.verbose)
        fmt::print("Parsed {} Tupfiles\n", state.parsed.size());
    auto num_commands = std::size_t { graph.nodes_of_type(pup::NodeType::Command).size() };

    if (num_commands == 0) {
        fmt::print("Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    auto index_path = layout.index_path();
    auto old_index = std::optional<pup::index::Index> {};
    auto use_incremental = false;
    auto changed_files = std::vector<std::string> {};

    if (std::filesystem::exists(index_path)) {
        auto reader_result = pup::Result<pup::index::IndexReader> { pup::index::IndexReader::open(index_path) };
        if (reader_result) {
            auto index_result = pup::Result<pup::index::Index> { reader_result->read() };
            if (index_result) {
                old_index = std::move(*index_result);
                changed_files = find_changed_files_with_implicit(layout.source_root, *old_index);
                changed_files = expand_implicit_deps(changed_files, *old_index, graph);

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
        .source_root = layout.source_root,
        .output_root = layout.output_root,
        .variant_dir = variant_dir,
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
            discovered_deps[job.id] = result.discovered_deps;
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
        build_result = scheduler.build_incremental(graph, *old_index, changed_files);
    } else {
        build_result = scheduler.build(graph);
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
        auto index = pup::index::Index { build_index(graph, discovered_deps, layout.source_root) };
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
    if (opts.command == "graph")
        return cmd_graph(opts);
    if (opts.command == "build")
        return cmd_build(opts);
    if (opts.command == "clean")
        return cmd_clean(opts);
    if (opts.command == "variant")
        return cmd_variant(opts);

    fmt::print(stderr, "Unknown command: {}\n", opts.command);
    print_usage();
    return EXIT_FAILURE;
}
