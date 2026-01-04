// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/gcc.hpp"

using namespace pup::graph;

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
            .command = "gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto const* scanner = registry.find_match(cmd);
        REQUIRE(scanner != nullptr);
        REQUIRE(scanner->name() == "gcc");
    }

    SECTION("returns nullptr for non-matching command")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = "ar rcs libfoo.a foo.o",
            .display = "AR libfoo.a",
            .inputs = { "foo.o" },
            .order_only_inputs = {},
            .outputs = { "libfoo.a" },
            .working_dir = ".",
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
            .command = "gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto rules = registry.match_and_generate(cmd);
        REQUIRE(rules.size() == 1);
        REQUIRE(rules[0].command == "gcc -M foo.c");
        REQUIRE(rules[0].action == OutputAction::InjectImplicitDeps);
        REQUIRE(rules[0].parent_command == 10);
    }

    SECTION("skips command with existing dep flags")
    {
        auto cmd = CommandInfo {
            .node_id = 11,
            .command = "gcc -MD -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto rules = registry.match_and_generate(cmd);
        REQUIRE(rules.empty());
    }

    SECTION("returns empty for non-matching command")
    {
        auto cmd = CommandInfo {
            .node_id = 12,
            .command = "ar rcs libfoo.a foo.o",
            .display = "AR libfoo.a",
            .inputs = { "foo.o" },
            .order_only_inputs = {},
            .outputs = { "libfoo.a" },
            .working_dir = ".",
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
        REQUIRE(spec.depfile_suffix == ".d");
    }

    SECTION("matches gcc compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = "gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };
        REQUIRE(scanner.matches(cmd));
    }

    SECTION("matches clang compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = "clang++ -c foo.cpp -o foo.o",
            .display = "CXX foo.o",
            .inputs = { "foo.cpp" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };
        REQUIRE(scanner.matches(cmd));
    }

    SECTION("does not match link command")
    {
        auto cmd = CommandInfo {
            .node_id = 3,
            .command = "gcc foo.o -o foo",
            .display = "LINK foo",
            .inputs = { "foo.o" },
            .order_only_inputs = {},
            .outputs = { "foo" },
            .working_dir = ".",
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
            .command = "gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "gcc -M foo.c");
    }

    SECTION("build_dep_command returns nullopt for empty command")
    {
        auto cmd = CommandInfo {
            .node_id = 5,
            .command = "",
            .display = "",
            .inputs = {},
            .order_only_inputs = {},
            .outputs = {},
            .working_dir = ".",
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
            .command = "ccache gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        REQUIRE(scanner.matches(cmd));
        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "ccache gcc -M foo.c");
    }

    SECTION("handles distcc wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = "distcc g++ -c foo.cpp -o foo.o",
            .display = "CXX foo.o",
            .inputs = { "foo.cpp" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "distcc g++ -M foo.cpp");
    }
}

TEST_CASE("GccScanner flag preservation", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("preserves include paths (combined form)")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = "gcc -I../include -I/usr/local/include -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "gcc -M -I../include -I/usr/local/include foo.c");
    }

    SECTION("preserves include paths (separate argument form)")
    {
        auto cmd = CommandInfo {
            .node_id = 10,
            .command = "gcc -I include -I ../lib -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "gcc -M -I include -I ../lib foo.c");
    }

    SECTION("preserves defines")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = "gcc -DNDEBUG -DFOO=bar -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "gcc -M -DNDEBUG -DFOO=bar foo.c");
    }

    SECTION("preserves -std flag")
    {
        auto cmd = CommandInfo {
            .node_id = 3,
            .command = "g++ -std=c++20 -c foo.cpp -o foo.o",
            .display = "CXX foo.o",
            .inputs = { "foo.cpp" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "g++ -M -std=c++20 foo.cpp");
    }

    SECTION("strips irrelevant flags")
    {
        auto cmd = CommandInfo {
            .node_id = 4,
            .command = "gcc -Wall -Wextra -O2 -g -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "gcc -M foo.c");
    }
}

TEST_CASE("GccScanner Objective-C support", "[dep_scanner][gcc]")
{
    auto scanner = scanners::GccScanner {};

    SECTION("build_dep_command handles Objective-C files")
    {
        auto cmd = CommandInfo {
            .node_id = 1,
            .command = "clang -c foo.m -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.m" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "clang -M foo.m");
    }

    SECTION("build_dep_command handles Objective-C++ files")
    {
        auto cmd = CommandInfo {
            .node_id = 2,
            .command = "clang++ -c bar.mm -o bar.o",
            .display = "CXX bar.o",
            .inputs = { "bar.mm" },
            .order_only_inputs = {},
            .outputs = { "bar.o" },
            .working_dir = ".",
        };

        auto dep_cmd = scanner.build_dep_command(cmd);
        REQUIRE(dep_cmd.has_value());
        REQUIRE(*dep_cmd == "clang++ -M bar.mm");
    }
}

TEST_CASE("make_gcc_scanner factory", "[dep_scanner][gcc]")
{
    auto scanner = scanners::make_gcc_scanner();
    REQUIRE(scanner != nullptr);
    REQUIRE(scanner->name() == "gcc");
}
