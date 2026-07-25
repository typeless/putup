// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/path_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "e2e_fixture.hpp"
#include "pup/cli/context.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/clang_cl.hpp"
#include "pup/graph/scanners/dep_words.hpp"
#include "pup/graph/scanners/gcc.hpp"
#include "shell_words.hpp"

#include <cstddef>
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

        auto const* scanner = registry.find_match(cmd);
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

        auto const* scanner = registry.find_match(cmd);
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

        auto rules = registry.match_and_generate(cmd);
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

        auto rules = registry.match_and_generate(cmd);
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

        auto rules = registry.match_and_generate(cmd);
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
        REQUIRE(scanner.matches(cmd));
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
        REQUIRE(scanner.matches(cmd));
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
        REQUIRE(!scanner.matches(cmd));
    }

    SECTION("has_dep_flags detects -MD")
    {
        REQUIRE(scanner.has_dep_flags("gcc -MD -c foo.c -o foo.o"));
    }

    SECTION("has_dep_flags detects -MMD")
    {
        REQUIRE(scanner.has_dep_flags("gcc -MMD -c foo.c -o foo.o"));
    }

    SECTION("has_dep_flags detects -MF")
    {
        REQUIRE(scanner.has_dep_flags("gcc -MF deps.d -c foo.c -o foo.o"));
    }

    SECTION("has_dep_flags returns false for normal compile")
    {
        REQUIRE(!scanner.has_dep_flags("gcc -c foo.c -o foo.o"));
    }

    SECTION("build_dep_command returns command")
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("gcc -M foo.c"));
    }

    SECTION("build_dep_command returns nullopt for empty command")
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(!dep_cmd.has_value());
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

        REQUIRE(scanner.matches(cmd));
        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("ccache gcc -M foo.c"));
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("distcc g++ -M foo.cpp"));
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("gcc -M -I../include -I/usr/local/include foo.c"));
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("gcc -M -I include -I ../lib foo.c"));
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("gcc -M -DNDEBUG -DFOO=bar foo.c"));
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("g++ -M -std=c++20 foo.cpp"));
    }

    SECTION("strips irrelevant flags")
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("gcc -M foo.c"));
    }
}

TEST_CASE("GccScanner Objective-C support", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("build_dep_command handles Objective-C files")
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("clang -M foo.m"));
    }

    SECTION("build_dep_command handles Objective-C++ files")
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("clang++ -M bar.mm"));
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(!dep_cmd.has_value());
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(!dep_cmd.has_value());
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

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(!dep_cmd.has_value());
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

        REQUIRE(scanner.matches(cmd));
        REQUIRE(!scanner.build_dep_command(cmd).has_value());
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
        REQUIRE(scanner.matches(clang_cl_compile(1, "clang-cl /std:c++latest -c foo.cpp -o foo.obj")));
    }

    SECTION("matches clang-cl compile with cl-spelled /c")
    {
        REQUIRE(scanner.matches(clang_cl_compile(2, "clang-cl /std:c++latest /c foo.cpp /Fofoo.obj")));
    }

    SECTION("matches versioned and .exe driver names")
    {
        REQUIRE(scanner.matches(clang_cl_compile(3, "clang-cl-20 -c foo.cpp -o foo.obj")));
        REQUIRE(scanner.matches(clang_cl_compile(4, "/usr/bin/clang-cl.exe -c foo.cpp -o foo.obj")));
    }

    SECTION("does not match a link command")
    {
        REQUIRE(!scanner.matches(clang_cl_compile(5, "clang-cl foo.obj -o foo.exe")));
    }

    SECTION("gcc scanner does not claim clang-cl commands")
    {
        auto gcc = scanners::GccScanner {};
        REQUIRE(!gcc.matches(clang_cl_compile(6, "clang-cl -c foo.cpp -o foo.obj")));
    }
}

TEST_CASE("ClangClScanner dep-flag detection", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};

    SECTION("detects a depfile whose path is pinned with -MF")
    {
        REQUIRE(scanner.has_dep_flags("clang-cl /clang:-MD /clang:-MFfoo.obj.d -c foo.cpp -o foo.obj"));
    }

    SECTION("bare -MD does not count: the depfile lands in the cwd, not beside the object")
    {
        REQUIRE(!scanner.has_dep_flags("clang-cl /clang:-MD -c foo.cpp -o foo.obj"));
        REQUIRE(!scanner.has_dep_flags("clang-cl /clang:-MMD -c foo.cpp -o foo.obj"));
    }

    SECTION("CRT-selection flags are not dep flags")
    {
        REQUIRE(!scanner.has_dep_flags("clang-cl /MT -c foo.cpp -o foo.obj"));
        REQUIRE(!scanner.has_dep_flags("clang-cl /MDd -c foo.cpp -o foo.obj"));
        REQUIRE(!scanner.has_dep_flags("clang-cl -MD -c foo.cpp -o foo.obj"));
        REQUIRE(!scanner.has_dep_flags("clang-cl -MT -c foo.cpp -o foo.obj"));
    }

    SECTION("plain compile has no dep flags")
    {
        REQUIRE(!scanner.has_dep_flags("clang-cl /W3 -c foo.cpp -o foo.obj"));
    }
}

TEST_CASE("ClangClScanner dep command construction", "[dep_scanner][clang_cl]")
{
    auto scanner = scanners::ClangClScanner {};

    SECTION("dep_spec returns stdout mode")
    {
        REQUIRE(scanner.dep_spec().output_mode == DepOutputMode::Stdout);
    }

    SECTION("drops compile-only flags and output paths")
    {
        auto dep_cmd = scanner.build_dep_command(
            clang_cl_compile(1, "clang-cl /W3 /WX /O2 /GR- /utf-8 /MT -c foo.cpp -o foo.obj")
        );
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("clang-cl /clang:-M foo.cpp"));
    }

    SECTION("drops cl-spelled /c and /Fo")
    {
        auto dep_cmd = scanner.build_dep_command(clang_cl_compile(2, "clang-cl /c foo.cpp /Fofoo.obj"));
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("clang-cl /clang:-M foo.cpp"));
    }

    SECTION("preserves include, define and standard flags in both spellings")
    {
        auto dep_cmd = scanner.build_dep_command(clang_cl_compile(
            3, "clang-cl /std:c++latest -Isrc /I../include -DUNICODE /DFOO=bar -c foo.cpp -o foo.obj"
        ));
        REQUIRE(dep_cmd.has_value());
        REQUIRE(
            *dep_cmd == intern("clang-cl /clang:-M /std:c++latest -Isrc /I../include -DUNICODE /DFOO=bar foo.cpp")
        );
    }

    SECTION("preserves separate-argument system include paths")
    {
        auto dep_cmd = scanner.build_dep_command(
            clang_cl_compile(4, "clang-cl /imsvc /sdk/crt/include /imsvc /sdk/um -c foo.cpp -o foo.obj")
        );
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("clang-cl /clang:-M /imsvc /sdk/crt/include /imsvc /sdk/um foo.cpp"));
    }

    SECTION("preserves target triple and force-includes")
    {
        auto dep_cmd = scanner.build_dep_command(clang_cl_compile(
            5, "clang-cl --target=x86_64-pc-windows-msvc /FI build/version.h -c foo.cpp -o foo.obj"
        ));
        REQUIRE(dep_cmd.has_value());
        REQUIRE(
            *dep_cmd == intern("clang-cl /clang:-M --target=x86_64-pc-windows-msvc /FI build/version.h foo.cpp")
        );
    }

    SECTION("returns nullopt for a compound shell command")
    {
        auto dep_cmd = scanner.build_dep_command(clang_cl_compile(6, "cd build && clang-cl -c foo.cpp -o foo.obj"));
        REQUIRE(!dep_cmd.has_value());
    }

    SECTION("drops a smuggled -MD that would redirect the depfile")
    {
        auto dep_cmd = scanner.build_dep_command(
            clang_cl_compile(8, "clang-cl /clang:-MD -Isrc -c foo.cpp -o foo.obj")
        );
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == intern("clang-cl /clang:-M -Isrc foo.cpp"));
    }

    SECTION("returns nullopt when no source word is visible")
    {
        auto dep_cmd = scanner.build_dep_command(clang_cl_compile(7, "clang-cl @srcs.rsp -c -o foo.obj"));
        REQUIRE(!dep_cmd.has_value());
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
        auto rules = registry.match_and_generate(clang_cl_compile(1, "clang-cl -Isrc -c foo.cpp -o foo.obj"));
        REQUIRE(rules.size() == 1);
        REQUIRE(rules[0].command == intern("clang-cl /clang:-M -Isrc foo.cpp"));
        REQUIRE(rules[0].action == OutputAction::InjectImplicitDeps);
        REQUIRE(rules[0].parent_command == 1);
    }

    SECTION("clang-cl compile that already emits a depfile putup can find gets none")
    {
        auto rules = registry.match_and_generate(
            clang_cl_compile(2, "clang-cl /clang:-MD /clang:-MFfoo.obj.d -c foo.cpp -o foo.obj")
        );
        REQUIRE(rules.empty());
    }

    SECTION("a source-less clang-cl compile gets no rule rather than a failing one")
    {
        auto rules = registry.match_and_generate(clang_cl_compile(3, "clang-cl @srcs.rsp -c -o foo.obj"));
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
    auto dep_cmd = scanner.build_dep_command(clang_cl_compile(
        9, "clang-cl /imsvc \"C:/Program Files/LLVM/lib/clang/20/include\" -c foo.cpp -o foo.obj"
    ));
    REQUIRE(dep_cmd.has_value());

    auto command = pup::global_pool().get(*dep_cmd);
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

TEST_CASE("Default scanner registry covers both compiler drivers", "[dep_scanner][clang_cl]")
{
    auto env = pup::test::EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
    auto registry = pup::cli::make_scanner_registry();
    REQUIRE(registry.has_value());

    SECTION("a gcc compile finds the gcc scanner")
    {
        auto cmd = clang_cl_compile(1, "g++ -c foo.cpp -o foo.o");
        auto const* scanner = registry->find_match(cmd);
        REQUIRE(scanner != nullptr);
        REQUIRE(scanner->name() == "gcc");
    }

    SECTION("a clang-cl compile finds the clang-cl scanner")
    {
        auto cmd = clang_cl_compile(2, "clang-cl -c foo.cpp -o foo.obj");
        auto const* scanner = registry->find_match(cmd);
        REQUIRE(scanner != nullptr);
        REQUIRE(scanner->name() == "clang-cl");
    }
}
