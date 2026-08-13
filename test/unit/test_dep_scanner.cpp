// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/path_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "e2e_fixture.hpp"
#include "pup/cli/context.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/clang_cl.hpp"
#include "pup/graph/scanners/dep_words.hpp"
#include "pup/graph/scanners/gcc.hpp"
#include "pup/platform/process.hpp"
#include "shell_words.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace pup::graph;

namespace {
auto intern(std::string_view s) -> pup::StringId { return pup::global_pool().intern(s); }
auto path(std::string_view s) -> pup::PathId
{
    static auto paths = pup::PathPool {};
    return paths.intern_path(s, pup::global_pool(), pup::PathId::BuildRoot);
}

/// Tokens for a command, built the one legal way. Tests hold them only for the call they make.
auto tokens_of(CommandInfo const& cmd) -> CommandTokens
{
    return tokenize_command(cmd.command);
}

auto tokens_of(std::string_view command) -> CommandTokens
{
    return tokenize_command(pup::global_pool().intern(command));
}

/// Whether a scanner claims a command, tokenized the one legal way.
auto scanner_matches(DepScanner const& scanner, CommandInfo const& cmd) -> bool
{
    return scanner.matches(cmd, tokens_of(cmd));
}

/// The scans a command builds and the rules a registry generates, tokenized the one legal way.
auto scans_of(DepScanner const& scanner, CommandInfo const& cmd) -> pup::Vec<DepScan>
{
    return scanner.build_dep_scans(cmd, tokens_of(cmd));
}

auto generated_for(DepScannerRegistry const& registry, CommandInfo const& cmd)
    -> pup::Vec<GeneratedRule>
{
    return registry.match_and_generate(cmd, tokens_of(cmd));
}

/// The one scan a command is expected to produce, asserting that it produces exactly one.
auto only_scan(DepScanner const& scanner, CommandInfo const& cmd) -> DepScan
{
    auto scans = scanner.build_dep_scans(cmd, tokens_of(cmd));
    REQUIRE(scans.size() == 1);
    return scans[0];
}
} // namespace

TEST_CASE("DepScannerRegistry basic operations", "[dep_scanner]")
{
    auto registry = DepScannerRegistry {};

    SECTION("empty registry")
    {
        REQUIRE(registry.empty());
        REQUIRE(registry.size() == 0);
    }

    SECTION("register scanner")
    {
        registry.register_scanner(scanners::make_gcc_scanner());
        REQUIRE(!registry.empty());
        REQUIRE(registry.size() == 1);
    }

    SECTION("register multiple scanners")
    {
        registry.register_scanner(scanners::make_gcc_scanner());
        registry.register_scanner(scanners::make_gcc_scanner());
        REQUIRE(registry.size() == 2);
    }
}

TEST_CASE("DepScannerRegistry find_match", "[dep_scanner]")
{
    auto registry = DepScannerRegistry {};
    registry.register_scanner(scanners::make_gcc_scanner());

    SECTION("finds scanner for gcc command")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = intern("gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto const* scanner = registry.find_match(cmd, tokens_of(cmd));
        REQUIRE(scanner != nullptr);
        REQUIRE(scanner->name() == "gcc");
    }

    SECTION("returns nullptr for non-matching command")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = intern("ar rcs libfoo.a foo.o"),
            .display = intern("AR libfoo.a"),
            .inputs = { intern("foo.o") },
            .order_only_inputs = {},
            .outputs = { path("libfoo.a") },
            .working_dir = intern("."),
        };

        auto const* scanner = registry.find_match(cmd, tokens_of(cmd));
        REQUIRE(scanner == nullptr);
    }
}

TEST_CASE("DepScannerRegistry match_and_generate", "[dep_scanner]")
{
    auto registry = DepScannerRegistry {};
    registry.register_scanner(scanners::make_gcc_scanner());

    SECTION("generates rule for gcc command")
    {
        auto cmd = CommandInfo {
            .node_id = 10,
            .command = intern("gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto rules = registry.match_and_generate(cmd, tokens_of(cmd));
        REQUIRE(rules.size() == 1);
        REQUIRE(rules[0].command == intern("gcc -M foo.c"));
        REQUIRE(rules[0].action == OutputAction::InjectImplicitDeps);
        REQUIRE(rules[0].parent_command == 10);
    }

    SECTION("skips command with existing dep flags")
    {
        auto cmd = CommandInfo {
            .node_id = 11,
            .command = intern("gcc -MD -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto rules = registry.match_and_generate(cmd, tokens_of(cmd));
        REQUIRE(rules.empty());
    }

    SECTION("returns empty for non-matching command")
    {
        auto cmd = CommandInfo {
            .node_id = 12,
            .command = intern("ar rcs libfoo.a foo.o"),
            .display = intern("AR libfoo.a"),
            .inputs = { intern("foo.o") },
            .order_only_inputs = {},
            .outputs = { path("libfoo.a") },
            .working_dir = intern("."),
        };

        auto rules = registry.match_and_generate(cmd, tokens_of(cmd));
        REQUIRE(rules.empty());
    }
}

TEST_CASE("GccScanner interface", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("name returns gcc")
    {
        REQUIRE(scanner.name() == "gcc");
    }

    SECTION("dep_spec returns stdout mode")
    {
        auto spec = scanner.dep_spec();
        REQUIRE(spec.output_mode == DepOutputMode::Stdout);
    }

    SECTION("matches gcc compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = intern("gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };
        REQUIRE(scanner.matches(cmd, tokens_of(cmd)));
    }

    SECTION("matches clang compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = intern("clang++ -c foo.cpp -o foo.o"),
            .display = intern("CXX foo.o"),
            .inputs = { intern("foo.cpp") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };
        REQUIRE(scanner.matches(cmd, tokens_of(cmd)));
    }

    SECTION("does not match link command")
    {
        auto cmd = CommandInfo {
            .node_id = 3,
            .command = intern("gcc foo.o -o foo"),
            .display = intern("LINK foo"),
            .inputs = { intern("foo.o") },
            .order_only_inputs = {},
            .outputs = { path("foo") },
            .working_dir = intern("."),
        };
        REQUIRE(!scanner.matches(cmd, tokens_of(cmd)));
    }

    SECTION("has_dep_flags detects -MD")
    {
        REQUIRE(scanner.has_dep_flags(tokens_of("gcc -MD -c foo.c -o foo.o")));
    }

    SECTION("has_dep_flags detects -MMD")
    {
        REQUIRE(scanner.has_dep_flags(tokens_of("gcc -MMD -c foo.c -o foo.o")));
    }

    SECTION("has_dep_flags detects -MF")
    {
        REQUIRE(scanner.has_dep_flags(tokens_of("gcc -MF deps.d -c foo.c -o foo.o")));
    }

    SECTION("has_dep_flags returns false for normal compile")
    {
        REQUIRE(!scanner.has_dep_flags(tokens_of("gcc -c foo.c -o foo.o")));
    }

    SECTION("build_dep_scans returns the scan")
    {
        auto cmd = CommandInfo {
            .node_id = 4,
            .command = intern("gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("gcc -M foo.c"));
    }

    SECTION("build_dep_scans returns nothing for empty command")
    {
        auto cmd = CommandInfo {
            .node_id = 5,
            .command = intern(""),
            .display = intern(""),
            .inputs = {},
            .order_only_inputs = {},
            .outputs = {},
            .working_dir = intern("."),
        };

        auto scans = scanner.build_dep_scans(cmd, tokens_of(cmd));
        REQUIRE(scans.empty());
    }
}

TEST_CASE("GccScanner compiler wrapper handling", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("handles ccache wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = intern("ccache gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        REQUIRE(scanner.matches(cmd, tokens_of(cmd)));
        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("ccache gcc -M foo.c"));
    }

    SECTION("handles distcc wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = intern("distcc g++ -c foo.cpp -o foo.o"),
            .display = intern("CXX foo.o"),
            .inputs = { intern("foo.cpp") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("distcc g++ -M foo.cpp"));
    }
}

TEST_CASE("GccScanner flag preservation", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("preserves include paths (combined form)")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = intern("gcc -I../include -I/usr/local/include -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("gcc -M -I../include -I/usr/local/include foo.c"));
    }

    SECTION("preserves include paths (separate argument form)")
    {
        auto cmd = CommandInfo {
            .node_id = 10,
            .command = intern("gcc -I include -I ../lib -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("gcc -M -I include -I ../lib foo.c"));
    }

    SECTION("preserves defines")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = intern("gcc -DNDEBUG -DFOO=bar -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("gcc -M -DNDEBUG -DFOO=bar foo.c"));
    }

    SECTION("preserves -std flag")
    {
        auto cmd = CommandInfo {
            .node_id = 3,
            .command = intern("g++ -std=c++20 -c foo.cpp -o foo.o"),
            .display = intern("CXX foo.o"),
            .inputs = { intern("foo.cpp") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("g++ -M -std=c++20 foo.cpp"));
    }

    SECTION("carries the compile's other flags")
    {
        auto cmd = CommandInfo {
            .node_id = 4,
            .command = intern("gcc -Wall -Wextra -O2 -g -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("gcc -M -Wall -Wextra -O2 -g foo.c"));
    }
}

TEST_CASE("GccScanner Objective-C support", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("build_dep_scans handles Objective-C files")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = intern("clang -c foo.m -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.m") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("clang -M foo.m"));
    }

    SECTION("build_dep_scans handles Objective-C++ files")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = intern("clang++ -c bar.mm -o bar.o"),
            .display = intern("CXX bar.o"),
            .inputs = { intern("bar.mm") },
            .order_only_inputs = {},
            .outputs = { path("bar.o") },
            .working_dir = intern("."),
        };

        auto scan = only_scan(scanner, cmd);
        REQUIRE(scan.command == intern("clang++ -M bar.mm"));
    }
}

TEST_CASE("GccScanner rejects compound shell commands", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("for loop with embedded compilation")
    {
        auto cmd = CommandInfo {
            .node_id = 20,
            .command = intern("for f in archive bfd cache; do gcc -O2 -c /src/bfd/$f.c -o out/bfd-$f.o || exit 1; done"),
            .display = intern("CC-BFD (3 files)"),
            .inputs = {},
            .order_only_inputs = {},
            .outputs = { path("bfd-archive.o"), path("bfd-bfd.o"), path("bfd-cache.o") },
            .working_dir = intern("."),
        };

        auto scans = scanner.build_dep_scans(cmd, tokens_of(cmd));
        REQUIRE(scans.empty());
    }

    SECTION("cd-and-compile compound command")
    {
        auto cmd = CommandInfo {
            .node_id = 21,
            .command = intern("cd /build && gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = {},
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scans = scanner.build_dep_scans(cmd, tokens_of(cmd));
        REQUIRE(scans.empty());
    }

    SECTION("env-var assignment before compiler")
    {
        auto cmd = CommandInfo {
            .node_id = 22,
            .command = intern("SRCDIR=$PWD && cd /build && gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = {},
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto scans = scanner.build_dep_scans(cmd, tokens_of(cmd));
        REQUIRE(scans.empty());
    }
}

TEST_CASE("GccScanner skips commands with no visible source", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("sources hidden in a response file yield no dep command")
    {
        auto cmd = CommandInfo {
            .node_id = 30,
            .command = intern("gcc @srcs.rsp -c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("srcs.rsp") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        REQUIRE(scanner.matches(cmd, tokens_of(cmd)));
        REQUIRE(scanner.build_dep_scans(cmd, tokens_of(cmd)).empty());
    }
}

TEST_CASE("make_gcc_scanner factory", "[dep_scanner][gcc]")
{
    auto scanner = scanners::make_gcc_scanner();
    REQUIRE(scanner != nullptr);
    REQUIRE(scanner->name() == "gcc");
}

namespace {
auto clang_cl_compile(pup::NodeId id, std::string_view command) -> CommandInfo
{
    return CommandInfo {
        .node_id = id,
        .command = intern(command),
        .display = intern("CXX foo.obj"),
        .inputs = { intern("foo.cpp") },
        .order_only_inputs = {},
        .outputs = { path("foo.obj") },
        .working_dir = intern("."),
    };
}
} // namespace

TEST_CASE("ClangClScanner matching", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};

    SECTION("name returns clang-cl")
    {
        REQUIRE(scanner.name() == "clang-cl");
    }

    SECTION("matches clang-cl compile with GNU-spelled -c")
    {
        REQUIRE(scanner_matches(scanner, clang_cl_compile(1, "clang-cl /std:c++latest -c foo.cpp -o foo.obj")));
    }

    SECTION("matches clang-cl compile with cl-spelled /c")
    {
        REQUIRE(scanner_matches(scanner, clang_cl_compile(2, "clang-cl /std:c++latest /c foo.cpp /Fofoo.obj")));
    }

    SECTION("matches versioned and .exe driver names")
    {
        REQUIRE(scanner_matches(scanner, clang_cl_compile(3, "clang-cl-20 -c foo.cpp -o foo.obj")));
        REQUIRE(scanner_matches(scanner, clang_cl_compile(4, "/usr/bin/clang-cl.exe -c foo.cpp -o foo.obj")));
    }

    SECTION("does not match a link command")
    {
        REQUIRE(!scanner_matches(scanner, clang_cl_compile(5, "clang-cl foo.obj -o foo.exe")));
    }

    SECTION("gcc scanner does not claim clang-cl commands")
    {
        auto gcc = scanners::GccScanner {};
        REQUIRE(!scanner_matches(gcc, clang_cl_compile(6, "clang-cl -c foo.cpp -o foo.obj")));
    }
}

TEST_CASE("ClangClScanner dep-flag detection", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};

    SECTION("detects a depfile whose path is pinned with -MF")
    {
        REQUIRE(scanner.has_dep_flags(tokens_of("clang-cl /clang:-MD /clang:-MFfoo.obj.d -c foo.cpp -o foo.obj")));
    }

    SECTION("bare -MD does not count: the depfile lands in the cwd, not beside the object")
    {
        REQUIRE(!scanner.has_dep_flags(tokens_of("clang-cl /clang:-MD -c foo.cpp -o foo.obj")));
        REQUIRE(!scanner.has_dep_flags(tokens_of("clang-cl /clang:-MMD -c foo.cpp -o foo.obj")));
    }

    SECTION("CRT-selection flags are not dep flags")
    {
        REQUIRE(!scanner.has_dep_flags(tokens_of("clang-cl /MT -c foo.cpp -o foo.obj")));
        REQUIRE(!scanner.has_dep_flags(tokens_of("clang-cl /MDd -c foo.cpp -o foo.obj")));
        REQUIRE(!scanner.has_dep_flags(tokens_of("clang-cl -MD -c foo.cpp -o foo.obj")));
        REQUIRE(!scanner.has_dep_flags(tokens_of("clang-cl -MT -c foo.cpp -o foo.obj")));
    }

    SECTION("plain compile has no dep flags")
    {
        REQUIRE(!scanner.has_dep_flags(tokens_of("clang-cl /W3 -c foo.cpp -o foo.obj")));
    }
}

TEST_CASE("ClangClScanner dep command construction", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};

    SECTION("dep_spec returns stdout mode")
    {
        REQUIRE(scanner.dep_spec().output_mode == DepOutputMode::Stdout);
    }

    SECTION("carries compile flags and drops output paths")
    {
        auto scan = only_scan(scanner, 
            clang_cl_compile(1, "clang-cl /W3 /WX /O2 /GR- /utf-8 /MT -c foo.cpp -o foo.obj")
        );
        REQUIRE(scan.command == intern("clang-cl /clang:-M /W3 /WX /O2 /GR- /utf-8 /MT foo.cpp"));
    }

    SECTION("drops cl-spelled /c and /Fo")
    {
        auto scan = only_scan(scanner, clang_cl_compile(2, "clang-cl /c foo.cpp /Fofoo.obj"));
        REQUIRE(scan.command == intern("clang-cl /clang:-M foo.cpp"));
    }

    SECTION("preserves include, define and standard flags in both spellings")
    {
        auto scan = only_scan(scanner, clang_cl_compile(
            3, "clang-cl /std:c++latest -Isrc /I../include -DUNICODE /DFOO=bar -c foo.cpp -o foo.obj"
        ));
        REQUIRE(
            scan.command == intern("clang-cl /clang:-M /std:c++latest -Isrc /I../include -DUNICODE /DFOO=bar foo.cpp")
        );
    }

    SECTION("preserves separate-argument system include paths")
    {
        auto scan = only_scan(scanner, 
            clang_cl_compile(4, "clang-cl /imsvc /sdk/crt/include /imsvc /sdk/um -c foo.cpp -o foo.obj")
        );
        REQUIRE(scan.command == intern("clang-cl /clang:-M /imsvc /sdk/crt/include /imsvc /sdk/um foo.cpp"));
    }

    SECTION("preserves target triple and force-includes")
    {
        auto scan = only_scan(scanner, clang_cl_compile(
            5, "clang-cl --target=x86_64-pc-windows-msvc /FI build/version.h -c foo.cpp -o foo.obj"
        ));
        REQUIRE(
            scan.command == intern("clang-cl /clang:-M --target=x86_64-pc-windows-msvc /FI build/version.h foo.cpp")
        );
    }

    SECTION("returns nullopt for a compound shell command")
    {
        auto scans = scans_of(scanner, clang_cl_compile(6, "cd build && clang-cl -c foo.cpp -o foo.obj"));
        REQUIRE(scans.empty());
    }

    SECTION("drops a smuggled -MD that would redirect the depfile")
    {
        auto scan = only_scan(scanner, 
            clang_cl_compile(8, "clang-cl /clang:-MD -Isrc -c foo.cpp -o foo.obj")
        );
        REQUIRE(scan.command == intern("clang-cl /clang:-M -Isrc foo.cpp"));
    }

    SECTION("returns nullopt when no source word is visible")
    {
        auto scans = scans_of(scanner, clang_cl_compile(7, "clang-cl @srcs.rsp -c -o foo.obj"));
        REQUIRE(scans.empty());
    }
}

TEST_CASE("make_clang_cl_scanner factory", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::make_clang_cl_scanner();
    REQUIRE(scanner != nullptr);
    REQUIRE(scanner->name() == "clang-cl");
}

TEST_CASE("Registry generates dep rules for clang-cl", "[dep_scanner][clang_cl]")
{
    auto registry = DepScannerRegistry {};
    registry.register_scanner(scanners::make_gcc_scanner());
    registry.register_scanner(scanners::make_clang_cl_scanner());

    SECTION("clang-cl compile gets one dep rule")
    {
        auto rules = generated_for(registry, clang_cl_compile(1, "clang-cl -Isrc -c foo.cpp -o foo.obj"));
        REQUIRE(rules.size() == 1);
        REQUIRE(rules[0].command == intern("clang-cl /clang:-M -Isrc foo.cpp"));
        REQUIRE(rules[0].action == OutputAction::InjectImplicitDeps);
        REQUIRE(rules[0].parent_command == 1);
    }

    SECTION("clang-cl compile that already emits a depfile putup can find gets none")
    {
        auto rules = generated_for(registry,
            clang_cl_compile(2, "clang-cl /clang:-MD /clang:-MFfoo.obj.d -c foo.cpp -o foo.obj")
        );
        REQUIRE(rules.empty());
    }

    SECTION("a source-less clang-cl compile gets no rule rather than a failing one")
    {
        auto rules = generated_for(registry, clang_cl_compile(3, "clang-cl @srcs.rsp -c -o foo.obj"));
        REQUIRE(rules.empty());
    }
}

namespace {

auto quote(std::string_view word, scanners::QuoteStyle style) -> std::string
{
    auto out = pup::Buf {};
    scanners::shell_quote_into(out, word, style);
    return std::string { out.view() };
}

struct ShellRoundTrip {
    std::string command;
    std::string output;
    int exit_code = 0;
    std::vector<std::string> argv;
};

/// Quotes `words`, spawns them through the same path a build job takes, and
/// reports the argv the child actually received.
auto round_trip_through_shell(std::vector<std::string> const& words) -> std::optional<ShellRoundTrip>
{
    auto command = pup::Buf {};
    scanners::shell_quote_into(command, pup::test::test_executable());
    command += " --dump-argv";
    for (auto const& word : words) {
        command += ' ';
        scanners::shell_quote_into(command, word);
    }

    auto proc = pup::platform::spawn_async(
        pup::platform::SpawnOptions { .command = command.view(), .working_dir = {} }
    );
    if (!proc) {
        return std::nullopt;
    }

    auto out = std::string {};
    auto buf = std::array<char, 1024> {};
    auto pollable = pup::platform::PollableFd { .fd = proc->stdout_fd, .is_stderr = false, .slot_index = 0 };
    for (auto spins = 0; spins < 2000; ++spins) {
        auto n = pup::platform::read_nonblocking(proc->stdout_fd, buf.data(), buf.size());
        if (n > 0) {
            out.append(buf.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        static_cast<void>(pup::platform::poll_fds(&pollable, 1, 10));
    }
    pup::platform::close_fd(proc->stdout_fd);
    pup::platform::close_fd(proc->stderr_fd);

    auto status = pup::platform::ProcessStatus {};
    pup::platform::reap(proc->pid, status);

    auto result = ShellRoundTrip {
        .command = std::string { command.view() },
        .output = out,
        .exit_code = status.exit_code,
        .argv = {},
    };
    auto start = std::size_t { 0 };
    while (start < out.size()) {
        auto end = out.find('\n', start);
        if (end == std::string::npos) {
            end = out.size();
        }
        auto line = out.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        result.argv.push_back(line);
        start = end + 1;
    }
    return result;
}

} // namespace

TEST_CASE("shell_quote_into quotes for the shell that will run the command", "[dep_scanner][quoting]")
{
    SECTION("a word with nothing special stays bare in both shells")
    {
        REQUIRE(quote("-Isrc/include", scanners::QuoteStyle::Posix) == "-Isrc/include");
        REQUIRE(quote("-Isrc/include", scanners::QuoteStyle::Windows) == "-Isrc/include");
    }

    SECTION("cmd.exe has no single-quote syntax, so a spaced path gets double quotes")
    {
        REQUIRE(
            quote("C:/Program Files/LLVM/lib/clang/20/include", scanners::QuoteStyle::Windows)
            == "\"C:/Program Files/LLVM/lib/clang/20/include\""
        );
        REQUIRE(
            quote("C:/Program Files/LLVM/lib/clang/20/include", scanners::QuoteStyle::Posix)
            == "'C:/Program Files/LLVM/lib/clang/20/include'"
        );
    }

    SECTION("a trailing backslash is doubled so it cannot escape the closing quote")
    {
        REQUIRE(quote("C:\\Program Files\\LLVM\\", scanners::QuoteStyle::Windows) == "\"C:\\Program Files\\LLVM\\\\\"");
    }

    SECTION("interior backslashes are not doubled")
    {
        REQUIRE(
            quote("C:\\Program Files\\LLVM\\include", scanners::QuoteStyle::Windows)
            == "\"C:\\Program Files\\LLVM\\include\""
        );
    }

    SECTION("an embedded quote is escaped the way its own shell expects")
    {
        REQUIRE(quote("a\"b c", scanners::QuoteStyle::Windows) == "\"a\\\"b c\"");
        REQUIRE(quote("a'b c", scanners::QuoteStyle::Posix) == "'a'\\''b c'");
    }

    SECTION("caret is cmd.exe's escape character, so it forces quoting")
    {
        REQUIRE(quote("-Ia^b", scanners::QuoteStyle::Windows) == "\"-Ia^b\"");
        REQUIRE(quote("-Ia^b", scanners::QuoteStyle::Posix) == "'-Ia^b'");
    }

    SECTION("an empty word stays one empty argument instead of vanishing")
    {
        REQUIRE(quote("", scanners::QuoteStyle::Posix) == "''");
        REQUIRE(quote("", scanners::QuoteStyle::Windows) == "\"\"");
    }
}

TEST_CASE("shell_quote_into round-trips through its target shell", "[dep_scanner][quoting]")
{
    static constexpr auto alphabet = std::string_view { "a \"\\'$" };
    static constexpr auto max_len = std::size_t { 4 };

    for (auto style : { scanners::QuoteStyle::Posix, scanners::QuoteStyle::Windows }) {
        for (auto len = std::size_t { 0 }; len <= max_len; ++len) {
            auto count = std::size_t { 1 };
            for (auto i = std::size_t { 0 }; i < len; ++i) {
                count *= alphabet.size();
            }
            for (auto n = std::size_t { 0 }; n < count; ++n) {
                auto word = std::string {};
                auto rest = n;
                for (auto i = std::size_t { 0 }; i < len; ++i) {
                    word += alphabet[rest % alphabet.size()];
                    rest /= alphabet.size();
                }

                auto quoted = quote(word, style);
                auto parsed = pup::test::split_for(quoted, style);

                INFO(
                    "style=" << (style == scanners::QuoteStyle::Posix ? "posix" : "windows") << " word=[" << word
                             << "] quoted=[" << quoted << "]"
                );
                REQUIRE(parsed.has_value());
                REQUIRE(parsed->size() == 1);
                REQUIRE((*parsed)[0] == word);
            }
        }
    }
}

TEST_CASE("ClangClScanner keeps a spaced include path in one argument", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};
    auto scan = only_scan(scanner, clang_cl_compile(
        9, "clang-cl /imsvc \"C:/Program Files/LLVM/lib/clang/20/include\" -c foo.cpp -o foo.obj"
    ));

    auto command = pup::global_pool().get(scan.command);
    auto words = pup::test::split_for_host(command);

    INFO("dep command: " << command);
    REQUIRE(words.has_value());
    REQUIRE(
        *words
        == std::vector<std::string> {
            "clang-cl",
            "/clang:-M",
            "/imsvc",
            "C:/Program Files/LLVM/lib/clang/20/include",
            "foo.cpp",
        }
    );
}

TEST_CASE("quoted words reach the child intact through the real shell", "[dep_scanner][quoting]")
{
    auto words = std::vector<std::string> {
        "C:/Program Files/LLVM/lib/clang/20/include",
        R"(-DGREETING="hello world")",
        "-Ia^b&c",
        R"(C:\Program Files\LLVM\)",
        "-I$SOMEDIR/include",
    };

    auto result = round_trip_through_shell(words);
    REQUIRE(result.has_value());

    INFO("command: " << result->command << "\nexit: " << result->exit_code << "\noutput: " << result->output);
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->argv == words);
}

TEST_CASE("Default scanner registry covers both compiler drivers", "[dep_scanner][clang_cl]")
{
    auto env = pup::test::EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
    auto registry = pup::cli::make_scanner_registry();
    REQUIRE(registry.has_value());

    SECTION("a gcc compile finds the gcc scanner")
    {
        auto cmd = clang_cl_compile(1, "g++ -c foo.cpp -o foo.o");
        auto const* scanner = registry->find_match(cmd, tokens_of(cmd));
        REQUIRE(scanner != nullptr);
        REQUIRE(scanner->name() == "gcc");
    }

    SECTION("a clang-cl compile finds the clang-cl scanner")
    {
        auto cmd = clang_cl_compile(2, "clang-cl -c foo.cpp -o foo.obj");
        auto const* scanner = registry->find_match(cmd, tokens_of(cmd));
        REQUIRE(scanner != nullptr);
        REQUIRE(scanner->name() == "clang-cl");
    }
}

namespace {

auto gcc_compile(pup::NodeId id, std::string_view command) -> CommandInfo
{
    return CommandInfo {
        .node_id = id,
        .command = intern(command),
        .display = intern("CC foo.o"),
        .inputs = { intern("foo.c") },
        .order_only_inputs = {},
        .outputs = { path("foo.o") },
        .working_dir = intern("."),
    };
}

auto scan_words(DepScan const& scan) -> std::vector<std::string>
{
    auto words = pup::test::split_for_host(pup::global_pool().get(scan.command));
    REQUIRE(words.has_value());
    return *words;
}

// Four call sites route through path::normalize by convention; this is what checks they still do.
auto check_scanners_normalize(std::string const& p) -> void
{
    auto const want = std::string { pup::global_pool().get(pup::path::normalize(p)) };
    INFO("path: " << p << "\npath::normalize: " << want);

    auto gcc = scanners::GccScanner {};
    REQUIRE(
        scan_words(only_scan(gcc, gcc_compile(30, "gcc -I" + p + " -c foo.c -o foo.o")))
        == std::vector<std::string> { "gcc", "-M", "-I" + want, "foo.c" }
    );
    REQUIRE(
        scan_words(only_scan(gcc, gcc_compile(32, "gcc -I " + p + " -c foo.c -o foo.o")))
        == std::vector<std::string> { "gcc", "-M", "-I", want, "foo.c" }
    );

    auto clang_cl = scanners::ClangClScanner {};
    REQUIRE(
        scan_words(only_scan(clang_cl, 
            clang_cl_compile(31, "clang-cl /imsvc " + p + " -c foo.cpp -o foo.obj")
        ))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "/imsvc", want, "foo.cpp" }
    );
    REQUIRE(
        scan_words(only_scan(clang_cl, 
            clang_cl_compile(33, "clang-cl -I" + p + " -c foo.cpp -o foo.obj")
        ))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "-I" + want, "foo.cpp" }
    );
}

// The sysroot spellings: the joined one carries its path after '=', the separate one a word later.
auto check_scanners_normalize_sysroot(std::string const& p) -> void
{
    auto const want = std::string { pup::global_pool().get(pup::path::normalize(p)) };
    INFO("path: " << p << "\npath::normalize: " << want);

    auto gcc = scanners::GccScanner {};
    REQUIRE(
        scan_words(only_scan(gcc, gcc_compile(44, "gcc --sysroot " + p + " -c foo.c -o foo.o")))
        == std::vector<std::string> { "gcc", "-M", "--sysroot", want, "foo.c" }
    );
    REQUIRE(
        scan_words(only_scan(gcc, gcc_compile(45, "gcc --sysroot=" + p + " -c foo.c -o foo.o")))
        == std::vector<std::string> { "gcc", "-M", "--sysroot=" + want, "foo.c" }
    );

    auto clang_cl = scanners::ClangClScanner {};
    REQUIRE(
        scan_words(only_scan(clang_cl, 
            clang_cl_compile(46, "clang-cl --sysroot " + p + " -c foo.cpp -o foo.obj")
        ))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "--sysroot", want, "foo.cpp" }
    );
    REQUIRE(
        scan_words(only_scan(clang_cl, 
            clang_cl_compile(47, "clang-cl --sysroot=" + p + " -c foo.cpp -o foo.obj")
        ))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "--sysroot=" + want, "foo.cpp" }
    );
}

// A macro definition is the preprocessor's, not a path: the same corpus, asserted verbatim.
auto check_scanners_pass_values_through(std::string const& p) -> void
{
    auto const want = "FOO=" + p;
    INFO("value: " << want);

    auto gcc = scanners::GccScanner {};
    REQUIRE(
        scan_words(only_scan(gcc, gcc_compile(34, "gcc -D " + want + " -c foo.c -o foo.o")))
        == std::vector<std::string> { "gcc", "-M", "-D", want, "foo.c" }
    );

    auto clang_cl = scanners::ClangClScanner {};
    REQUIRE(
        scan_words(only_scan(clang_cl, 
            clang_cl_compile(35, "clang-cl /D " + want + " -c foo.cpp -o foo.obj")
        ))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "/D", want, "foo.cpp" }
    );
}

auto gcc_scan_words(std::string const& command) -> std::vector<std::string>
{
    auto scanner = scanners::GccScanner {};
    return scan_words(only_scan(scanner, gcc_compile(50, command)));
}

auto clang_cl_scan_words(std::string const& command) -> std::vector<std::string>
{
    auto scanner = scanners::ClangClScanner {};
    return scan_words(only_scan(scanner, clang_cl_compile(51, command)));
}

struct WalkTarget {
    std::string_view driver;
    std::string_view scan_marker;
    std::string_view source;
    std::string_view object;
    scanners::FlagTables tables;
    std::vector<std::string> (*scan)(std::string const&);
};

// The tables say which arguments to normalize and consume and which words must not ride along.
auto check_flag_table_walk(WalkTarget const& t) -> void
{
    auto const payload = std::string { "a/./b/../c" };
    auto const normalized = std::string { pup::global_pool().get(pup::path::normalize(payload)) };

    auto expect = [&t](std::vector<std::string> const& flag_words) {
        auto words = std::vector<std::string> { std::string { t.driver }, std::string { t.scan_marker } };
        words.insert(words.end(), flag_words.begin(), flag_words.end());
        words.emplace_back(t.source);
        return words;
    };
    auto command = [&t](std::string const& flags) {
        return std::string { t.driver } + " " + flags + " -c " + std::string { t.source } + " -o "
            + std::string { t.object };
    };

    for (auto const& flag : t.tables.joined) {
        auto const word = std::string { flag.spelling } + payload;
        auto const want = flag.kind == scanners::SeparateArg::Path
            ? std::string { flag.spelling } + normalized
            : word;
        INFO("joined flag: " << flag.spelling);
        REQUIRE(t.scan(command(word)) == expect({ want }));
    }

    for (auto const& flag : t.tables.separate) {
        auto const want = flag.kind == scanners::SeparateArg::Path ? normalized : payload;
        INFO("separate flag: " << flag.spelling);
        REQUIRE(t.scan(command(std::string { flag.spelling } + " " + payload)) == expect({ std::string { flag.spelling }, want }));
    }

    for (auto spelling : t.tables.hazards) {
        auto const word = std::string { spelling };
        INFO("hazard flag: " << word);
        REQUIRE(t.scan(command(word)) == expect({}));
    }
}

// The scan is the compile minus the words that would break it: forwarded in order, hazards gone,
// the output flag and its argument gone, sources last.
auto check_scan_forwards_the_compile(std::string const& p) -> void
{
    auto const want = std::string { pup::global_pool().get(pup::path::normalize(p)) };
    INFO("path: " << p);

    auto gcc = scanners::GccScanner {};
    REQUIRE(
        scan_words(only_scan(gcc, 
            gcc_compile(54, "gcc -O2 -MD -I" + p + " -Wall -c foo.c -o foo.o")
        ))
        == std::vector<std::string> { "gcc", "-M", "-O2", "-I" + want, "-Wall", "foo.c" }
    );

    auto clang_cl = scanners::ClangClScanner {};
    REQUIRE(
        scan_words(only_scan(clang_cl, 
            clang_cl_compile(55, "clang-cl /O2 /showIncludes /imsvc " + p + " /W4 -c foo.cpp -o foo.obj")
        ))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "/O2", "/imsvc", want, "/W4", "foo.cpp" }
    );
}

auto check_under_root(std::string_view root, void (*check)(std::string const&)) -> void
{
    auto const alphabet = std::array<std::string_view, 5> { "a", "b", ".", "..", "" };
    for (auto x : alphabet) {
        auto const one = std::string { root } + std::string { x };
        if (!one.empty()) {
            check(one);
        }
        for (auto y : alphabet) {
            auto const two = one + "/" + std::string { y };
            check(two);
            for (auto z : alphabet) {
                check(two + "/" + std::string { z });
            }
        }
    }
}

} // namespace

TEST_CASE("GccScanner resolves a root-escaping include path", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(20, "gcc -I/a/../.. -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -I/ foo.c");
}

TEST_CASE("a scanned path flag says what path::normalize says", "[dep_scanner]")
{
    SECTION("relative paths")
    {
        check_under_root("", check_scanners_normalize);
    }

    SECTION("rooted paths")
    {
        check_under_root("/", check_scanners_normalize);
    }

    SECTION("drive-rooted paths")
    {
        check_under_root("C:/", check_scanners_normalize);
    }
}

TEST_CASE("a scanned value flag passes its word through", "[dep_scanner]")
{
    SECTION("relative-looking values")
    {
        check_under_root("", check_scanners_pass_values_through);
    }

    SECTION("rooted values")
    {
        check_under_root("/", check_scanners_pass_values_through);
    }

    SECTION("drive-rooted values")
    {
        check_under_root("C:/", check_scanners_pass_values_through);
    }
}

TEST_CASE("GccScanner passes a separate-word macro definition through", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(36, "gcc -D FOO=a/../b -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -D FOO=a/../b foo.c");
}

TEST_CASE("GccScanner keeps a macro value that is only a slash", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(37, "gcc -D ROOT=/ -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -D ROOT=/ foo.c");
}

TEST_CASE("a macro value needing quotes survives the scan command", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(38, R"(gcc -D "P=a b/../c" -c foo.c -o foo.o)"));

    REQUIRE(
        scan_words(scan) == std::vector<std::string> { "gcc", "-M", "-D", "P=a b/../c", "foo.c" }
    );
}

TEST_CASE("ClangClScanner passes separate-word macro words through", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};

    REQUIRE(
        scan_words(only_scan(scanner, clang_cl_compile(39, "clang-cl /D FOO=a/../b -c foo.cpp -o foo.obj")))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "/D", "FOO=a/../b", "foo.cpp" }
    );
    REQUIRE(
        scan_words(only_scan(scanner, clang_cl_compile(40, "clang-cl -D FOO=a/../b -c foo.cpp -o foo.obj")))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "-D", "FOO=a/../b", "foo.cpp" }
    );
    REQUIRE(
        scan_words(only_scan(scanner, clang_cl_compile(41, "clang-cl /U FOO -c foo.cpp -o foo.obj")))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "/U", "FOO", "foo.cpp" }
    );
    REQUIRE(
        scan_words(only_scan(scanner, clang_cl_compile(42, "clang-cl -U FOO -c foo.cpp -o foo.obj")))
        == std::vector<std::string> { "clang-cl", "/clang:-M", "-U", "FOO", "foo.cpp" }
    );
}

TEST_CASE("every flag a scanner knows carries its argument as its row says", "[dep_scanner]")
{
    SECTION("gcc")
    {
        check_flag_table_walk(WalkTarget {
            .driver = "gcc",
            .scan_marker = "-M",
            .source = "foo.c",
            .object = "foo.o",
            .tables = scanners::gcc_flag_tables(),
            .scan = gcc_scan_words,
        });
    }

    SECTION("clang-cl")
    {
        check_flag_table_walk(WalkTarget {
            .driver = "clang-cl",
            .scan_marker = "/clang:-M",
            .source = "foo.cpp",
            .object = "foo.obj",
            .tables = scanners::clang_cl_flag_tables(),
            .scan = clang_cl_scan_words,
        });
    }
}

TEST_CASE("a scan is the compile without the words that would break it", "[dep_scanner]")
{
    SECTION("relative paths")
    {
        check_under_root("", check_scan_forwards_the_compile);
    }

    SECTION("rooted paths")
    {
        check_under_root("/", check_scan_forwards_the_compile);
    }

    SECTION("drive-rooted paths")
    {
        check_under_root("C:/", check_scan_forwards_the_compile);
    }
}

TEST_CASE("GccScanner scans each compile of an all-compile command with its own flags", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scans = scans_of(scanner,
        gcc_compile(57, "gcc -O2 -c a.c -o a.o && gcc -c b.c -o b.o")
    );

    REQUIRE(scans.size() == 2);
    REQUIRE(pup::global_pool().get(scans[0].command) == "gcc -M -O2 a.c");
    REQUIRE(pup::global_pool().get(scans[0].object) == "a.o");
    REQUIRE(pup::global_pool().get(scans[1].command) == "gcc -M b.c");
    REQUIRE(pup::global_pool().get(scans[1].object) == "b.o");
}

TEST_CASE("GccScanner scans the prefix before a directory change", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scans = scans_of(scanner,
        gcc_compile(70, "gcc -c a.c -o a.o && cd sub && gcc -c b.c -o b.o")
    );

    REQUIRE(scans.size() == 1);
    REQUIRE(pup::global_pool().get(scans[0].command) == "gcc -M a.c");
    REQUIRE(pup::global_pool().get(scans[0].object) == "a.o");
}

TEST_CASE("GccScanner scans the prefix before an invocation that is not a compile", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("a later invocation that deletes a file contributes no source")
    {
        auto scans = scans_of(scanner, gcc_compile(71, "gcc -c a.c -o a.o && rm junk.c"));
        REQUIRE(scans.size() == 1);
        REQUIRE(pup::global_pool().get(scans[0].command) == "gcc -M a.c");
    }

    SECTION("a later invocation that copies files contributes neither operand")
    {
        auto scans = scans_of(scanner, gcc_compile(72, "gcc -c a.c -o a.o && cp b.c x.c"));
        REQUIRE(scans.size() == 1);
        REQUIRE(pup::global_pool().get(scans[0].command) == "gcc -M a.c");
    }
}

TEST_CASE("A scan reads the object from either spelling of the output flag", "[dep_scanner]")
{
    SECTION("gcc's joined form")
    {
        auto scanner = scanners::GccScanner {};
        auto scan = only_scan(scanner, gcc_compile(76, "gcc -c a.c -oa.o"));
        REQUIRE(pup::global_pool().get(scan.command) == "gcc -M a.c");
        REQUIRE(pup::global_pool().get(scan.object) == "a.o");
    }

    SECTION("clang-cl's joined form")
    {
        auto scanner = scanners::ClangClScanner {};
        auto scan = only_scan(scanner, clang_cl_compile(77, "clang-cl -c a.cpp -oa.obj"));
        REQUIRE(pup::global_pool().get(scan.command) == "clang-cl /clang:-M a.cpp");
        REQUIRE(pup::global_pool().get(scan.object) == "a.obj");
    }
}

TEST_CASE("A command whose first invocation is not a compile is scanned nowhere", "[dep_scanner][gcc]")
{
    // The bound that keeps the prefix rule from becoming scan-everything: an empty prefix, so no
    // later compile is reproducible from the rule's directory either.
    auto scanner = scanners::GccScanner {};
    REQUIRE(scans_of(scanner, gcc_compile(75, "cd sub && gcc -c a.c -o a.o")).empty());
}

TEST_CASE("A separator with nothing after it begins no invocation", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("a trailing terminator")
    {
        auto scan = only_scan(scanner, gcc_compile(74, "gcc -c a.c -o a.o ;"));
        REQUIRE(pup::global_pool().get(scan.command) == "gcc -M a.c");
    }

    SECTION("a trailing backgrounding operator")
    {
        REQUIRE(scanners::matches_gcc_compile("gcc -c a.c -o a.o &"));
    }

    SECTION("a leading separator begins an invocation that runs nothing")
    {
        REQUIRE(scanners::matches_gcc_compile("; gcc -c a.c -o a.o"));
    }
}

TEST_CASE("matches_gcc_compile accepts a command whose prefix compiles", "[dep_scanner][gcc]")
{
    // The diagnostic consumes the matcher and the scan consumes the builder; a rule they answer
    // differently about is a rule reported as covered and scanned wrongly.
    REQUIRE(scanners::matches_gcc_compile("gcc -c a.c -o a.o && rm junk.c"));
}

TEST_CASE("matches_gcc_compile refuses a command whose first invocation is not a compile", "[dep_scanner][gcc]")
{
    // An empty prefix, so nothing downstream of it is reproducible -- the bound that keeps the
    // prefix rule from becoming scan-everything.
    REQUIRE(!scanners::matches_gcc_compile("gcc -o prog main.c && gcc -c helper.c"));
}

TEST_CASE("matches_clang_cl_compile refuses a command whose first invocation is not a compile", "[dep_scanner][clang_cl]")
{
    REQUIRE(!scanners::matches_clang_cl_compile("clang-cl foo.obj -o foo.exe && clang-cl -c bar.cpp"));
}

TEST_CASE("ClangClScanner scans the prefix before an invocation that is not a compile", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};
    auto scans = scans_of(scanner,
        clang_cl_compile(73, "clang-cl -c a.cpp -o a.obj && cd sub && clang-cl -c b.cpp -o b.obj")
    );

    REQUIRE(scans.size() == 1);
    REQUIRE(pup::global_pool().get(scans[0].command) == "clang-cl /clang:-M a.cpp");
    REQUIRE(pup::global_pool().get(scans[0].object) == "a.obj");
}

TEST_CASE("GccScanner keeps a leading environment assignment in the scan", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("the assignment precedes the compiler in the scan")
    {
        auto scan = only_scan(scanner, gcc_compile(80, "FOO=1 gcc -c a.c -o a.o"));
        REQUIRE(pup::global_pool().get(scan.command) == "FOO=1 gcc -M a.c");
        REQUIRE(pup::global_pool().get(scan.object) == "a.o");
    }

    SECTION("a variable that moves the header search is kept, not dropped")
    {
        // Keeping it is what makes the scan sound: dropping CPATH would search a path the
        // compile never searched, which is #355's redirection in a new place.
        auto scan = only_scan(scanner, gcc_compile(81, "CPATH=inc gcc -c a.c -o a.o"));
        REQUIRE(pup::global_pool().get(scan.command) == "CPATH=inc gcc -M a.c");
    }

    SECTION("every assignment before a wrapper is kept")
    {
        auto scan = only_scan(scanner, gcc_compile(82, "A=1 B=2 ccache gcc -c a.c -o a.o"));
        REQUIRE(pup::global_pool().get(scan.command) == "A=1 B=2 ccache gcc -M a.c");
    }

    SECTION("the matcher answers as the builder does")
    {
        REQUIRE(scanners::matches_gcc_compile("FOO=1 gcc -c a.c -o a.o"));
    }

    SECTION("a standalone assignment is not absorbed")
    {
        // The prefix form scopes to one command; the standalone form persists into the rest of
        // the line, so no scan of a later invocation reproduces it.
        REQUIRE(!scanners::matches_gcc_compile("FOO=1; gcc -c a.c -o a.o"));
        REQUIRE(scans_of(scanner, gcc_compile(83, "FOO=1; gcc -c a.c -o a.o")).empty());
    }

    SECTION("a value the scan's own shell would expand a second time is not absorbed")
    {
        REQUIRE(!scanners::matches_gcc_compile("FOO=$(date) gcc -c a.c -o a.o"));
    }
}

TEST_CASE("GccScanner scans past an invocation that changes nothing", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("an invocation with no words")
    {
        auto scan = only_scan(scanner, gcc_compile(84, "; gcc -c a.c -o a.o"));
        REQUIRE(pup::global_pool().get(scan.command) == "gcc -M a.c");
    }

    SECTION("an announcement")
    {
        auto scan = only_scan(scanner, gcc_compile(85, "echo building a.c && gcc -c a.c -o a.o"));
        REQUIRE(pup::global_pool().get(scan.command) == "gcc -M a.c");
    }

    SECTION("the other programs that do nothing")
    {
        REQUIRE(scanners::matches_gcc_compile("true && gcc -c a.c -o a.o"));
        REQUIRE(scanners::matches_gcc_compile(": && gcc -c a.c -o a.o"));
    }

    SECTION("an announcement that writes a file is opaque")
    {
        // The write may create the very header the compile reads.
        REQUIRE(!scanners::matches_gcc_compile("echo x > f.h && gcc -c f.c -o f.o"));
        REQUIRE(scans_of(scanner, gcc_compile(86, "echo x >f.h && gcc -c f.c -o f.o")).empty());
    }

    SECTION("an announcement that runs a program is opaque")
    {
        REQUIRE(!scanners::matches_gcc_compile("echo $(gen) && gcc -c a.c -o a.o"));
    }

    SECTION("a program that is neither compile nor announcement still ends the prefix")
    {
        REQUIRE(scans_of(scanner, gcc_compile(87, "cat x && gcc -c a.c -o a.o")).empty());
    }

    SECTION("a command that only announces is claimed by no scanner")
    {
        REQUIRE(!scanners::matches_gcc_compile("echo nothing to do"));
    }

    SECTION("an announcement after the last compile adds no scan")
    {
        auto scans = scans_of(scanner, gcc_compile(88, "gcc -c a.c -o a.o && echo done"));
        REQUIRE(scans.size() == 1);
        REQUIRE(pup::global_pool().get(scans[0].command) == "gcc -M a.c");
    }
}

TEST_CASE("ClangClScanner reads the same prefix its sibling does", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};

    SECTION("a leading assignment is kept in the scan")
    {
        auto scan = only_scan(scanner, clang_cl_compile(89, "INCLUDE=inc clang-cl -c a.cpp -o a.obj"));
        REQUIRE(pup::global_pool().get(scan.command) == "INCLUDE=inc clang-cl /clang:-M a.cpp");
        REQUIRE(pup::global_pool().get(scan.object) == "a.obj");
    }

    SECTION("an announcement does not end the prefix")
    {
        auto scan = only_scan(scanner, clang_cl_compile(90, "echo building && clang-cl -c a.cpp -o a.obj"));
        REQUIRE(pup::global_pool().get(scan.command) == "clang-cl /clang:-M a.cpp");
    }

    SECTION("an announcement that writes a file is opaque")
    {
        REQUIRE(!scanners::matches_clang_cl_compile("echo x > f.h && clang-cl -c f.cpp -o f.obj"));
    }

    SECTION("a standalone assignment is not absorbed")
    {
        REQUIRE(!scanners::matches_clang_cl_compile("FOO=1; clang-cl -c a.cpp -o a.obj"));
    }
}

TEST_CASE("a line continuation never reaches the scan command", "[dep_scanner]")
{
    SECTION("a continuation the command text kept is not a word the scan carries")
    {
        auto scanner = scanners::GccScanner {};
        auto scan = only_scan(scanner, 
            gcc_compile(60, "gcc -O2 \n -DFOO=1 -c foo.c -o foo.o \n ")
        );
        REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -O2 -DFOO=1 foo.c");
    }

    SECTION("a newline inside a word survives the shell that runs the scan")
    {
        auto scanner = scanners::GccScanner {};
        auto scan = only_scan(scanner, gcc_compile(61, "gcc -DA=a\nb -c foo.c -o foo.o"));
        REQUIRE(
            scan_words(scan) == std::vector<std::string> { "gcc", "-M", "-DA=a\nb", "foo.c" }
        );
    }

    SECTION("a carriage return from a Windows-authored Tupfile")
    {
        auto scanner = scanners::ClangClScanner {};
        auto scan = only_scan(scanner, 
            clang_cl_compile(62, "clang-cl /O2 \r\n /I inc -c foo.cpp -o foo.obj")
        );
        REQUIRE(pup::global_pool().get(scan.command) == "clang-cl /clang:-M /O2 /I inc foo.cpp");
    }
}

TEST_CASE("GccScanner takes no flags from a redirection", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("a numbered file descriptor")
    {
        auto scan = only_scan(scanner, gcc_compile(58, "gcc -O2 -c foo.c -o foo.o 1> build.log"));
        REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -O2 foo.c");
    }

    SECTION("a duplicated descriptor")
    {
        auto scan = only_scan(scanner, gcc_compile(59, "gcc -O2 -c foo.c -o foo.o 2>&1"));
        REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -O2 foo.c");
    }
}

TEST_CASE("ClangClScanner stops at the linker's share of the command", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};
    auto scan = only_scan(scanner, 
        clang_cl_compile(56, "clang-cl /I inc -c foo.cpp -o foo.obj /link /LIBPATH:C:/lib")
    );
    REQUIRE(pup::global_pool().get(scan.command) == "clang-cl /clang:-M /I inc foo.cpp");
}

TEST_CASE("GccScanner drops a macro the shell would expand and scans anyway", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(52, "gcc -DPATH=$(prefix)/share -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M foo.c");
}

TEST_CASE("GccScanner carries the compile's other flags into the scan", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(53, "gcc -O2 -Wall -Iinc -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -O2 -Wall -Iinc foo.c");
}

TEST_CASE("GccScanner keeps a separate-word sysroot path", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(48, "gcc --sysroot /opt/sys/../sys -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M --sysroot /opt/sys foo.c");
}

// gcc takes the word after --sysroot whatever it is, so the scan mirrors that greed.
TEST_CASE("GccScanner lets a value-less sysroot take the next word", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(49, "gcc --sysroot -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M --sysroot -c foo.c");
}

TEST_CASE("a scanned sysroot flag says what path::normalize says", "[dep_scanner]")
{
    SECTION("relative paths")
    {
        check_under_root("", check_scanners_normalize_sysroot);
    }

    SECTION("rooted paths")
    {
        check_under_root("/", check_scanners_normalize_sysroot);
    }

    SECTION("drive-rooted paths")
    {
        check_under_root("C:/", check_scanners_normalize_sysroot);
    }
}

TEST_CASE("GccScanner keeps a separate-word undefine", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};
    auto scan = only_scan(scanner, gcc_compile(43, "gcc -U FOO -c foo.c -o foo.o"));
    REQUIRE(pup::global_pool().get(scan.command) == "gcc -M -U FOO foo.c");
}

#ifdef _WIN32

TEST_CASE("ClangClScanner keeps the drive root in a scanned flag", "[dep_scanner][clang_cl][windows]")
{
    auto scanner = scanners::ClangClScanner {};

    SECTION("dotdot cannot escape the drive")
    {
        auto scan = only_scan(scanner, clang_cl_compile(21, "clang-cl /imsvc C:/.. -c foo.cpp -o foo.obj"));
        REQUIRE(
            scan_words(scan)
            == std::vector<std::string> { "clang-cl", "/clang:-M", "/imsvc", "C:/", "foo.cpp" }
        );
    }

    SECTION("the drive root stays rooted")
    {
        auto scan = only_scan(scanner, clang_cl_compile(22, "clang-cl /imsvc C:/ -c foo.cpp -o foo.obj"));
        REQUIRE(
            scan_words(scan)
            == std::vector<std::string> { "clang-cl", "/clang:-M", "/imsvc", "C:/", "foo.cpp" }
        );
    }
}

#endif
