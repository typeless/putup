// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/options.hpp"
#include "pup/core/platform.hpp"

#include <cstdlib>
#include <string_view>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto const VERSION = "0.1.0";

} // namespace

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

} // namespace pup::cli
