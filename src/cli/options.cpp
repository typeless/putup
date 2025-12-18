// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/options.hpp"
#include "pup/core/platform.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto const VERSION = "0.1.0";

auto is_command(std::string_view arg) -> bool
{
    return arg == "parse" || arg == "export" || arg == "clean" || arg == "distclean"
        || arg == "variant";
}

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
        } else if (arg == "--stat") {
            opts.stat = true;
        } else if (arg == "-k" || arg == "--keep-going") {
            opts.keep_going = true;
        } else if (arg == "-j" || arg == "--jobs") {
            if (i + 1 < argc) {
                auto const* str = argv[++i];
                auto value = int {};
                auto [ptr, ec] = std::from_chars(str, str + std::strlen(str), value);
                if (ec != std::errc {} || *ptr != '\0' || value <= 0) {
                    fmt::print(stderr, "Error: Invalid job count '{}'\n", str);
                    std::exit(EXIT_FAILURE);
                }
                opts.jobs = static_cast<std::size_t>(value);
            }
        } else if (arg.starts_with("-j")) {
            auto str = arg.substr(2);
            auto value = int {};
            auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
            if (ec != std::errc {} || ptr != str.data() + str.size() || value <= 0) {
                fmt::print(stderr, "Error: Invalid job count '{}'\n", str);
                std::exit(EXIT_FAILURE);
            }
            opts.jobs = static_cast<std::size_t>(value);
        } else if (arg == "-S") {
            if (i + 1 < argc) {
                opts.source_dir = std::string { argv[++i] };
            }
        } else if (arg == "-B") {
            if (i + 1 < argc) {
                opts.build_dirs.emplace_back(argv[++i]);
            }
        } else if (arg == "--summary") {
            opts.summary = true;
        } else if (arg == "-a" || arg == "--all-deps") {
            opts.include_all_deps = true;
        } else if (arg == "-A" || arg == "--all") {
            opts.all = true;
        } else if (arg == "--") {
            for (++i; i < argc; ++i) {
                opts.targets.emplace_back(argv[i]);
            }
        } else if (!arg.starts_with("-")) {
            if (opts.command.empty() && is_command(arg)) {
                opts.command = std::string { arg };
            } else if (opts.command == "export" && opts.export_format.empty()) {
                opts.export_format = std::string { arg };
            } else {
                opts.targets.emplace_back(arg);
            }
        }
    }

    return opts;
}

auto print_usage() -> void
{
    fmt::print("pup - build system using Tupfile format\n\n"
               "Usage: pup [OPTIONS] [TARGETS]\n"
               "       pup [OPTIONS] <command>\n\n"
               "Running 'pup' executes the build. Use a command for other operations.\n\n"
               "Commands:\n"
               "  parse             Parse and validate Tupfiles\n"
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
               "  -B DIR             Build/output directory (can use multiple times)\n"
               "  --summary          Human-readable output (for export graph)\n"
               "  --stat             Print build statistics\n"
               "  -A, --all          Full project build (ignore cwd scoping)\n"
               "  -a, --all-deps     Include upstream deps in scoped builds\n"
               "  --                 End of options; remaining args are targets\n"
               "  --version          Print version\n"
               "  -h, --help         Print this help\n"
               "\nTargets:\n"
               "  path/to/variant    Build specific variant (has tup.config)\n"
               "  path/to/dir        Scope build to directory\n"
               "  pattern-*          Glob to match multiple variants\n"
               "  variant/dir        Variant + directory scope\n"
               "\nExamples:\n"
               "  pup                Build all variants\n"
               "  pup build-debug    Build single variant\n"
               "  pup build-*        Build all matching variants\n"
               "  pup src/lib        Scoped build across all variants\n"
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
