// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/core/hash.hpp"
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
#include <sstream>
#include <string_view>

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
    std::vector<std::string> targets = {};
};

auto print_usage() -> void
{
    fmt::print("pup - Tup build system reimplementation\n\n"
               "Usage: pup [OPTIONS] [COMMAND]\n\n"
               "Commands:\n"
               "  init     Initialize .pup directory\n"
               "  parse    Parse and validate Tupfiles\n"
               "  build    Execute build (default)\n"
               "  graph    Print dependency graph\n"
               "\nOptions:\n"
               "  -j, --jobs N       Run N jobs in parallel\n"
               "  -k, --keep-going   Continue after failures\n"
               "  -n, --dry-run      Print commands without executing\n"
               "  -v, --verbose      Verbose output\n"
               "  --variant=DIR      Use DIR as variant output directory\n"
               "  --version          Print version\n"
               "  -h, --help         Print this help\n");
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

auto find_project_root() -> std::optional<std::filesystem::path>
{
    auto current = std::filesystem::path{std::filesystem::current_path()};

    while (true) {
        if (std::filesystem::exists(current / "Tupfile") || std::filesystem::exists(current / "Tuprules.tup") || std::filesystem::exists(current / PUP_DIR)) {
            return current;
        }

        auto parent = std::filesystem::path{current.parent_path()};
        if (parent == current)
            return std::nullopt;
        current = parent;
    }
}

auto find_variant_dir(std::filesystem::path const& root) -> std::optional<std::filesystem::path>
{
    for (auto const& name : { "build", "out", "variant" }) {
        auto dir = std::filesystem::path{root / name};
        if (std::filesystem::exists(dir / "tup.config"))
            return dir;
    }

    if (std::filesystem::is_directory(root)) {
        for (auto const& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory()) {
                auto config_path = std::filesystem::path{entry.path() / "tup.config"};
                if (std::filesystem::exists(config_path))
                    return entry.path();
            }
        }
    }

    return std::nullopt;
}

auto compute_variantdir(
    std::filesystem::path const& source_dir,
    std::filesystem::path const& variant_dir) -> std::string
{
    if (variant_dir.empty())
        return ".";

    auto output_dir = std::filesystem::path{variant_dir / source_dir};
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

auto cmd_init(Options const& /*opts*/) -> int
{
    auto root = std::filesystem::path{std::filesystem::current_path()};

    auto pup_dir = std::filesystem::path{root / PUP_DIR};
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
    auto root = std::optional<std::filesystem::path>{find_project_root()};
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project (no Tupfile found)\n");
        return EXIT_FAILURE;
    }

    if (opts.verbose)
        fmt::print("Project root: \"{}\"\n", root->string());

    auto tupfile_path = std::filesystem::path{*root / "Tupfile"};
    if (!std::filesystem::exists(tupfile_path)) {
        fmt::print(stderr, "Error: No Tupfile found in \"{}\"\n", root->string());
        return EXIT_FAILURE;
    }

    auto source = std::optional<std::string>{read_file(tupfile_path)};
    if (!source) {
        fmt::print(stderr, "Error: Failed to read Tupfile\n");
        return EXIT_FAILURE;
    }

    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto result = pup::Result<pup::parser::Tupfile>{parser.parse()};

    if (!result) {
        fmt::print(stderr, "Parse error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto const& tupfile = *result;
    fmt::print("Parsed {} statements from Tupfile\n", tupfile.statements.size());

    if (opts.verbose) {
        for (auto const& stmt : tupfile.statements) {
            if (auto const* rule = stmt->as<pup::parser::Rule>()) {
                fmt::print("  Rule: {} inputs -> {} outputs\n",
                    rule->inputs.size(), rule->outputs.size());
            } else if (auto const* assign = stmt->as<pup::parser::Assignment>()) {
                fmt::print("  Assignment: {}\n", assign->name);
            } else if (auto const* macro = stmt->as<pup::parser::BangMacro>()) {
                fmt::print("  Macro: !{}\n", macro->name);
            }
        }
    }

    return EXIT_SUCCESS;
}

auto cmd_graph(Options const& opts) -> int
{
    auto root = std::optional<std::filesystem::path>{find_project_root()};
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project\n");
        return EXIT_FAILURE;
    }

    auto tupfile_path = std::filesystem::path{*root / "Tupfile"};
    auto source = std::optional<std::string>{read_file(tupfile_path)};
    if (!source) {
        fmt::print(stderr, "Error: Failed to read Tupfile\n");
        return EXIT_FAILURE;
    }

    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto parse_result = pup::Result<pup::parser::Tupfile>{parser.parse()};
    if (!parse_result) {
        fmt::print(stderr, "Parse error: {}\n", parse_result.error().message);
        return EXIT_FAILURE;
    }

    auto vars = pup::parser::VarDb {};
    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .tup_cwd = std::string{root->string()},
        .tup_platform = std::string { pup::PLATFORM },
        .tup_arch = std::string { pup::ARCH },
    };

    auto builder_opts = pup::graph::BuilderOptions{
        .root_dir = *root,
        .variant_dir = {},
        .expand_globs = true,
    };

    auto builder = pup::graph::GraphBuilder{builder_opts};
    auto graph_result = pup::Result<pup::graph::BuildGraph>{builder.build(*parse_result, eval_ctx)};

    if (!graph_result) {
        fmt::print(stderr, "Graph build error: {}\n", graph_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& graph = *graph_result;
    auto num_nodes = std::size_t{graph.node_count()};
    auto num_edges = std::size_t{graph.edge_count()};
    auto commands = std::vector<pup::NodeId>{graph.nodes_of_type(pup::NodeType::Command)};
    fmt::print("Nodes: {}\n", num_nodes);
    fmt::print("Edges: {}\n", num_edges);
    fmt::print("Commands: {}\n", commands.size());

    if (opts.verbose) {
        fmt::print("\nCommands:\n");
        for (auto id : commands) {
            if (auto const* node = graph.get_node(id)) {
                auto display = std::string{node->display.empty() ? node->command : node->display};
                fmt::print("  {}\n", display);
            }
        }
    }

    return EXIT_SUCCESS;
}

auto cmd_build(Options const& opts) -> int
{
    auto root = std::optional<std::filesystem::path>{find_project_root()};
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project\n");
        return EXIT_FAILURE;
    }

    auto tupfile_path = std::filesystem::path{*root / "Tupfile"};
    auto source = std::optional<std::string>{read_file(tupfile_path)};
    if (!source) {
        fmt::print(stderr, "Error: Failed to read Tupfile\n");
        return EXIT_FAILURE;
    }

    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto parse_result = pup::Result<pup::parser::Tupfile>{parser.parse()};
    if (!parse_result) {
        fmt::print(stderr, "Parse error: {}\n", parse_result.error().message);
        return EXIT_FAILURE;
    }

    auto variant_dir = std::filesystem::path{};
    if (!opts.variant.empty()) {
        variant_dir = std::filesystem::path{opts.variant};
    } else {
        auto discovered = std::optional<std::filesystem::path>{find_variant_dir(*root)};
        if (discovered)
            variant_dir = std::filesystem::relative(*discovered, *root);
    }

    auto config_vars = pup::parser::VarDb{};
    if (!variant_dir.empty()) {
        auto config_path = std::filesystem::path{*root / variant_dir / "tup.config"};
        if (std::filesystem::exists(config_path)) {
            auto config_result = pup::Result<pup::parser::VarDb>{pup::parser::parse_config(config_path)};
            if (config_result) {
                config_vars = std::move(*config_result);
                if (opts.verbose)
                    fmt::print("Loaded config from {}\n", config_path.string());
            }
        }
    }

    auto vars = pup::parser::VarDb{};
    auto tup_variantdir = compute_variantdir(std::filesystem::path{}, variant_dir);
    auto eval_ctx = pup::parser::EvalContext{
        .vars = &vars,
        .config_vars = &config_vars,
        .tup_cwd = std::string{root->string()},
        .tup_platform = std::string{pup::PLATFORM},
        .tup_arch = std::string{pup::ARCH},
        .tup_variantdir = tup_variantdir,
        .tup_variant_outputdir = variant_dir.empty() ? "." : variant_dir.string(),
    };

    auto builder_opts = pup::graph::BuilderOptions{
        .root_dir = *root,
        .variant_dir = variant_dir,
        .expand_globs = true,
    };

    auto builder = pup::graph::GraphBuilder { builder_opts };
    auto graph_result = pup::Result<pup::graph::BuildGraph>{builder.build(*parse_result, eval_ctx)};

    if (!graph_result) {
        fmt::print(stderr, "Graph error: {}\n", graph_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& graph = *graph_result;
    auto num_commands = std::size_t{graph.nodes_of_type(pup::NodeType::Command).size()};

    if (num_commands == 0) {
        fmt::print("Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    auto sched_opts = pup::exec::SchedulerOptions {
        .jobs = opts.jobs,
        .keep_going = opts.keep_going,
        .dry_run = opts.dry_run,
        .verbose = opts.verbose,
        .root_dir = *root,
    };

    auto scheduler = pup::exec::Scheduler { sched_opts };

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run)
            fmt::print("{}\n", job.display);
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& result) {
        if (!result.success) {
            fmt::print(stderr, "FAILED: {}\n", job.display);
            if (!result.output.empty())
                fmt::print(stderr, "{}\n", result.output);
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

    auto start = std::chrono::steady_clock::time_point{std::chrono::steady_clock::now()};
    auto build_result = pup::Result<pup::exec::BuildStats>{scheduler.build(graph)};
    auto end = std::chrono::steady_clock::time_point{std::chrono::steady_clock::now()};
    auto duration = std::chrono::milliseconds{std::chrono::duration_cast<std::chrono::milliseconds>(end - start)};

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

    return stats.failed_jobs > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // anonymous namespace

auto main(int argc, char** argv) -> int
{
    auto opts = Options{parse_args(argc, argv)};

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

    fmt::print(stderr, "Unknown command: {}\n", opts.command);
    print_usage();
    return EXIT_FAILURE;
}
