// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/path_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/gcc.hpp"

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

TEST_CASE("make_gcc_scanner factory", "[dep_scanner][gcc]")
{
    auto scanner = scanners::make_gcc_scanner();
    REQUIRE(scanner != nullptr);
    REQUIRE(scanner->name() == "gcc");
}
