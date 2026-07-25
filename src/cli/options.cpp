// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/options.hpp"
#include "pup/cli/subcommand.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/print.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/ignore.hpp"
#include "pup/platform/env.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>

namespace pup::cli {

namespace {

auto const VERSION = "3.0.0";

auto is_command(std::string_view arg) -> bool
{
    return arg == "parse" || arg == "show" || arg == "clean" || arg == "distclean"
        || arg == "configure";
}

auto strip_config_prefix(std::string_view name) -> std::string_view
{
    constexpr auto CONFIG_PREFIX = std::string_view { "CONFIG_" };
    if (name.starts_with(CONFIG_PREFIX)) {
        return name.substr(CONFIG_PREFIX.size());
    }
    return name;
}

auto parse_define(std::string_view arg) -> std::pair<StringId, StringId>
{
    auto& pool = global_pool();
    auto eq = arg.find('=');
    if (eq == std::string_view::npos) {
        auto name = strip_config_prefix(arg);
        return { pool.intern(name), pool.intern("y") };
    }
    auto name = strip_config_prefix(arg.substr(0, eq));
    return { pool.intern(name), pool.intern(arg.substr(eq + 1)) };
}

} // namespace

auto parse_args(int argc, char** argv) -> Options
{
    auto opts = Options {};
    auto& pool = global_pool();
    auto saw_positional = false;

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
        } else if (arg == "--no-stat-cache") {
            opts.no_stat_cache = true;
        } else if (arg == "-j" || arg == "--jobs") {
            if (i + 1 < argc) {
                auto const* str = argv[++i];
                auto value = int {};
                auto [ptr, ec] = std::from_chars(str, str + std::strlen(str), value);
                if (ec != std::errc {} || *ptr != '\0' || value <= 0) {
                    eprint("Error: Invalid job count '{}'\n", str);
                    std::exit(EXIT_FAILURE);
                }
                opts.jobs = static_cast<std::size_t>(value);
            }
        } else if (arg.starts_with("-j")) {
            auto str = arg.substr(2);
            auto value = int {};
            auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
            if (ec != std::errc {} || ptr != str.data() + str.size() || value <= 0) {
                eprint("Error: Invalid job count '{}'\n", str);
                std::exit(EXIT_FAILURE);
            }
            opts.jobs = static_cast<std::size_t>(value);
        } else if (arg == "-S" || arg == "--source-dir") {
            if (i + 1 < argc) {
                opts.source_dir = pool.intern(argv[++i]);
            }
        } else if (arg == "-C" || arg == "--config-dir") {
            if (i + 1 < argc) {
                opts.config_dir = pool.intern(argv[++i]);
            }
        } else if (arg == "-B" || arg == "--build-dir") {
            if (i + 1 < argc) {
                opts.build_dirs.push_back(pool.intern(argv[++i]));
            }
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                opts.config_file = pool.intern(argv[++i]);
            }
        } else if (arg == "--summary") {
            opts.summary = true;
        } else if (arg == "-a" || arg == "--all-deps") {
            opts.include_all_deps = true;
        } else if (arg == "-A" || arg == "--all") {
            opts.all = true;
        } else if (arg == "-x" || arg == "--exclude") {
            if (i + 1 >= argc) {
                eprint("Error: {} requires an exclude pattern\n", arg);
                std::exit(EXIT_FAILURE);
            }
            auto pattern = std::string_view { argv[++i] };
            auto probe = pup::parser::IgnoreList {};
            probe.add(pattern);
            if (probe.empty()) {
                eprint("Error: invalid exclude pattern '{}'\n", pattern);
                std::exit(EXIT_FAILURE);
            }
            opts.excludes.push_back(pool.intern(pattern));
        } else if (arg == "-D" || arg == "--define") {
            if (i + 1 < argc) {
                opts.config_defines.push_back(parse_define(argv[++i]));
            }
        } else if (arg.starts_with("-D")) {
            opts.config_defines.push_back(parse_define(arg.substr(2)));
        } else if (arg == "--") {
            for (++i; i < argc; ++i) {
                opts.targets.push_back(pool.intern(argv[i]));
            }
        } else if (arg == "--json") {
            opts.show_json = true;
        } else if (arg == "--strict") {
            opts.check = CheckLevel::Error;
        } else if (arg.starts_with("--check=")) {
            auto level = arg.substr(std::string_view { "--check=" }.size());
            if (level == "none") {
                opts.check = CheckLevel::None;
            } else if (level == "warn") {
                opts.check = CheckLevel::Warn;
            } else if (level == "error") {
                opts.check = CheckLevel::Error;
            } else {
                eprint("Error: invalid --check level '{}' (expected none|warn|error)\n", level);
                std::exit(EXIT_FAILURE);
            }
        } else if (arg.starts_with("-")) {
            eprint("Error: unknown option '{}'\nRun 'putup --help' for usage.\n", argv[i]);
            std::exit(EXIT_FAILURE);
        } else if (!arg.starts_with("-")) {
            auto const first_positional = !std::exchange(saw_positional, true);
            auto const builtin = is_empty(opts.command) && is_command(arg);
            auto external = StringId::Empty;
            if (first_positional && !builtin) {
                auto const* search_path = platform::get_env("PATH");
                external = search_path ? find_subcommand(arg, search_path) : StringId::Empty;
            }

            if (builtin) {
                opts.command = pool.intern(arg);
            } else if (!is_empty(external)) {
                auto tail = Vec<StringId> {};
                for (++i; i < argc; ++i) {
                    tail.push_back(pool.intern(argv[i]));
                }
                opts.external = ExternalCommand { external, std::move(tail) };
            } else if (pool.get(opts.command) == "show" && is_empty(opts.show_format)) {
                opts.show_format = pool.intern(arg);
            } else if (pool.get(opts.command) == "show"
                       && (pool.get(opts.show_format) == "var" || pool.get(opts.show_format) == "index")
                       && is_empty(opts.show_var_filter)) {
                opts.show_var_filter = pool.intern(arg);
            } else {
                opts.targets.push_back(pool.intern(arg));
            }
        }
    }

    return opts;
}

auto print_usage() -> void
{
    print("putup - build system using Tupfile format\n\n"
          "Usage: putup [OPTIONS] [TARGETS]\n"
          "       putup [OPTIONS] <command>\n\n"
          "Running 'putup' executes the build. Use a command for other operations.\n\n"
          "Commands:\n"
          "  configure         Generate tup.config files (two-stage build)\n"
          "  clean             Remove generated files\n"
          "  distclean         Full reset: remove .pup and variant directory\n"
          "  parse [--check=LEVEL]\n"
          "                    Parse and validate Tupfiles\n"
          "  show <format>     Show build info:\n"
          "                      script  - Shell script\n"
          "                      compdb  - compile_commands.json\n"
          "                      graph   - DOT format (--summary for text)\n"
          "                      var [NAME] [--json] - Variable tracking\n"
          "                      index   - Index dump (--summary for counts only)\n"
          "\nExternal subcommands:\n"
          "  'putup NAME ARGS...' runs the executable 'putup-NAME' from PATH when NAME is\n"
          "  the first non-option argument, is not a builtin command, and such an\n"
          "  executable exists. Everything after NAME is passed on untouched; options\n"
          "  before it stay putup's own. Use 'putup -- NAME' to build a target instead.\n"
          "\nOptions:\n"
          "  -j, --jobs N       Run N jobs in parallel\n"
          "  -k, --keep-going   Continue after failures\n"
          "  --no-stat-cache    Hash every file's contents (skip the size+mtime fast path)\n"
          "  -n, --dry-run      Print commands without executing\n"
          "  -v, --verbose      Verbose output\n"
          "  -D, --define VAR=value\n"
          "                     Override config variable (-D VAR is shorthand for -D VAR=y)\n"
          "  -S DIR             Source directory (where source files live)\n"
          "  -C DIR             Config directory (where Tupfiles live)\n"
          "  -B DIR             Build/output directory (can use multiple times)\n"
          "  -c, --config FILE  Install FILE as root tup.config before config rules\n"
          "  --check=LEVEL      Convention checking for 'parse':\n"
          "                       none  - skip checks\n"
          "                       warn  - report violations, exit 0 (default)\n"
          "                       error - report violations, exit non-zero\n"
          "                     (--strict is an alias for --check=error)\n"
          "  --summary          Human-readable output (for show graph)\n"
          "  --stat             Print build statistics\n"
          "  -A, --all          Full project build (ignore cwd scoping)\n"
          "  -a, --all-deps     Include upstream deps in scoped builds\n"
          "  -x, --exclude PATTERN\n"
          "                     Skip building directories matching PATTERN (gitignore\n"
          "                     syntax); their index state is preserved, not discarded\n"
          "  --                 End of options; remaining args are targets\n"
          "  --version          Print version\n"
          "  -h, --help         Print this help\n"
          "\nTargets:\n"
          "  A variant is a directory containing tup.config. Anything else is a scope.\n\n"
          "  TARGET              VARIANT       SCOPE\n"
          "  build               build         (all)       # if build/tup.config exists\n"
          "  build/src/lib       build         src/lib\n"
          "  src/lib             (none)        src/lib     # no tup.config in src/\n"
          "  build/foo.o         build         foo.o       # single output rebuild\n"
          "  build-*             (glob)        (all)       # multiple variants\n"
          "\nExamples:\n"
          "  putup                Build in-tree, or the enclosing build dir when run inside one\n"
          "  putup build-debug    Build single variant\n"
          "  putup build-*        Build all matching variants\n"
          "  putup src/lib        Scoped build (variant selection rules above apply)\n"
          "\nEnvironment:\n"
          "  PUP_SOURCE_DIR     Source directory (overridden by -S)\n"
          "  PUP_CONFIG_DIR     Config directory (overridden by -C)\n"
          "  PUP_BUILD_DIR      Build directory (overridden by -B)\n"
          "  PUP_IMPLICIT_DEPS  Set to 0 to disable auto-generated dep rules (default: enabled)\n");
}

auto print_version() -> void
{
    print("putup {}\n", VERSION);
    print("Platform: {}\n", pup::PLATFORM);
    print("Architecture: {}\n", pup::ARCH);
}

} // namespace pup::cli
