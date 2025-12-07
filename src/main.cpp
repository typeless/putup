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
#include "pup/parser/parser.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

auto const VERSION = "0.1.0";
auto const PUP_DIR = ".pup";
// auto const INDEX_FILE = ".pup/index";  // For future incremental builds

struct Options {
    std::size_t jobs = 0;
    bool keep_going = false;
    bool verbose = false;
    bool dry_run = false;
    bool version = false;
    bool help = false;
    std::string command = {};
    std::vector<std::string> targets = {};
};

auto print_usage() -> void
{
    std::cout << "pup - Tup build system reimplementation\n\n";
    std::cout << "Usage: pup [OPTIONS] [COMMAND]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  init     Initialize .pup directory\n";
    std::cout << "  parse    Parse and validate Tupfiles\n";
    std::cout << "  build    Execute build (default)\n";
    std::cout << "  graph    Print dependency graph\n";
    std::cout << "\nOptions:\n";
    std::cout << "  -j, --jobs N     Run N jobs in parallel\n";
    std::cout << "  -k, --keep-going Continue after failures\n";
    std::cout << "  -n, --dry-run    Print commands without executing\n";
    std::cout << "  -v, --verbose    Verbose output\n";
    std::cout << "  --version        Print version\n";
    std::cout << "  -h, --help       Print this help\n";
}

auto print_version() -> void
{
    std::cout << "pup " << VERSION << "\n";
    std::cout << "Platform: " << pup::PLATFORM << "\n";
    std::cout << "Architecture: " << pup::ARCH << "\n";
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
        std::cout << "Already initialized in " << root << "\n";
        return EXIT_SUCCESS;
    }

    std::filesystem::create_directory(pup_dir);
    std::cout << "Initialized pup in " << root << "\n";
    return EXIT_SUCCESS;
}

auto cmd_parse(Options const& opts) -> int
{
    auto root = std::optional<std::filesystem::path>{find_project_root()};
    if (!root) {
        std::cerr << "Error: Not in a pup/tup project (no Tupfile found)\n";
        return EXIT_FAILURE;
    }

    if (opts.verbose)
        std::cout << "Project root: " << *root << "\n";

    auto tupfile_path = std::filesystem::path{*root / "Tupfile"};
    if (!std::filesystem::exists(tupfile_path)) {
        std::cerr << "Error: No Tupfile found in " << *root << "\n";
        return EXIT_FAILURE;
    }

    auto source = std::optional<std::string>{read_file(tupfile_path)};
    if (!source) {
        std::cerr << "Error: Failed to read Tupfile\n";
        return EXIT_FAILURE;
    }

    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto result = pup::Result<pup::parser::Tupfile>{parser.parse()};

    if (!result) {
        std::cerr << "Parse error: " << result.error().message << "\n";
        return EXIT_FAILURE;
    }

    auto const& tupfile = *result;
    std::cout << "Parsed " << tupfile.statements.size() << " statements from Tupfile\n";

    if (opts.verbose) {
        for (auto const& stmt : tupfile.statements) {
            if (auto const* rule = stmt->as<pup::parser::Rule>()) {
                std::cout << "  Rule: " << rule->inputs.size() << " inputs -> "
                          << rule->outputs.size() << " outputs\n";
            } else if (auto const* assign = stmt->as<pup::parser::Assignment>()) {
                std::cout << "  Assignment: " << assign->name << "\n";
            } else if (auto const* macro = stmt->as<pup::parser::BangMacro>()) {
                std::cout << "  Macro: !" << macro->name << "\n";
            }
        }
    }

    return EXIT_SUCCESS;
}

auto cmd_graph(Options const& opts) -> int
{
    auto root = std::optional<std::filesystem::path>{find_project_root()};
    if (!root) {
        std::cerr << "Error: Not in a pup/tup project\n";
        return EXIT_FAILURE;
    }

    auto tupfile_path = std::filesystem::path{*root / "Tupfile"};
    auto source = std::optional<std::string>{read_file(tupfile_path)};
    if (!source) {
        std::cerr << "Error: Failed to read Tupfile\n";
        return EXIT_FAILURE;
    }

    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto parse_result = pup::Result<pup::parser::Tupfile>{parser.parse()};
    if (!parse_result) {
        std::cerr << "Parse error: " << parse_result.error().message << "\n";
        return EXIT_FAILURE;
    }

    auto vars = pup::parser::VarDb {};
    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .tup_cwd = std::string{root->string()},
        .tup_platform = std::string { pup::PLATFORM },
        .tup_arch = std::string { pup::ARCH },
    };

    auto builder_opts = pup::graph::BuilderOptions {
        .root_dir = *root,
        .expand_globs = true,
    };

    auto builder = pup::graph::GraphBuilder { builder_opts };
    auto graph_result = pup::Result<pup::graph::BuildGraph>{builder.build(*parse_result, eval_ctx)};

    if (!graph_result) {
        std::cerr << "Graph build error: " << graph_result.error().message << "\n";
        return EXIT_FAILURE;
    }

    auto const& graph = *graph_result;
    auto num_nodes = std::size_t{graph.node_count()};
    auto num_edges = std::size_t{graph.edge_count()};
    auto commands = std::vector<pup::NodeId>{graph.nodes_of_type(pup::NodeType::Command)};
    std::cout << "Nodes: " << num_nodes << "\n";
    std::cout << "Edges: " << num_edges << "\n";
    std::cout << "Commands: " << commands.size() << "\n";

    if (opts.verbose) {
        std::cout << "\nCommands:\n";
        for (auto id : commands) {
            if (auto const* node = graph.get_node(id)) {
                auto display = std::string{node->display.empty() ? node->command : node->display};
                std::cout << "  " << display << "\n";
            }
        }
    }

    return EXIT_SUCCESS;
}

auto cmd_build(Options const& opts) -> int
{
    auto root = std::optional<std::filesystem::path>{find_project_root()};
    if (!root) {
        std::cerr << "Error: Not in a pup/tup project\n";
        return EXIT_FAILURE;
    }

    auto tupfile_path = std::filesystem::path{*root / "Tupfile"};
    auto source = std::optional<std::string>{read_file(tupfile_path)};
    if (!source) {
        std::cerr << "Error: Failed to read Tupfile\n";
        return EXIT_FAILURE;
    }

    // Parse
    auto parser = pup::parser::Parser { *source, tupfile_path.string() };
    auto parse_result = pup::Result<pup::parser::Tupfile>{parser.parse()};
    if (!parse_result) {
        std::cerr << "Parse error: " << parse_result.error().message << "\n";
        return EXIT_FAILURE;
    }

    // Build graph
    auto vars = pup::parser::VarDb {};
    auto eval_ctx = pup::parser::EvalContext {
        .vars = &vars,
        .tup_cwd = std::string{root->string()},
        .tup_platform = std::string { pup::PLATFORM },
        .tup_arch = std::string { pup::ARCH },
    };

    auto builder_opts = pup::graph::BuilderOptions {
        .root_dir = *root,
        .expand_globs = true,
    };

    auto builder = pup::graph::GraphBuilder { builder_opts };
    auto graph_result = pup::Result<pup::graph::BuildGraph>{builder.build(*parse_result, eval_ctx)};

    if (!graph_result) {
        std::cerr << "Graph error: " << graph_result.error().message << "\n";
        return EXIT_FAILURE;
    }

    auto const& graph = *graph_result;
    auto num_commands = std::size_t{graph.nodes_of_type(pup::NodeType::Command).size()};

    if (num_commands == 0) {
        std::cout << "Nothing to do.\n";
        return EXIT_SUCCESS;
    }

    // Set up scheduler
    auto sched_opts = pup::exec::SchedulerOptions {
        .jobs = opts.jobs,
        .keep_going = opts.keep_going,
        .dry_run = opts.dry_run,
        .verbose = opts.verbose,
        .root_dir = *root,
    };

    auto scheduler = pup::exec::Scheduler { sched_opts };

    // Progress display
    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run) {
            std::cout << job.display << "\n";
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& result) {
        if (!result.success) {
            std::cerr << "FAILED: " << job.display << "\n";
            if (!result.output.empty())
                std::cerr << result.output << "\n";
        }
    });

    auto completed = std::size_t { 0 };
    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        completed = done;
        if (!opts.verbose) {
            std::cout << "\r[" << done << "/" << total << "] " << std::flush;
        }
    });

    // Execute build
    auto start = std::chrono::steady_clock::time_point{std::chrono::steady_clock::now()};
    auto build_result = pup::Result<pup::exec::BuildStats>{scheduler.build(graph)};
    auto end = std::chrono::steady_clock::time_point{std::chrono::steady_clock::now()};
    auto duration = std::chrono::milliseconds{std::chrono::duration_cast<std::chrono::milliseconds>(end - start)};

    if (!opts.verbose)
        std::cout << "\n";

    if (!build_result) {
        std::cerr << "Build failed: " << build_result.error().message << "\n";
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    std::cout << "Build completed: " << stats.completed_jobs << " commands";
    if (stats.failed_jobs > 0)
        std::cout << " (" << stats.failed_jobs << " failed)";
    std::cout << " in " << duration.count() << "ms\n";

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

    std::cerr << "Unknown command: " << opts.command << "\n";
    print_usage();
    return EXIT_FAILURE;
}
