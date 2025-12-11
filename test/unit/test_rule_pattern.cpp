// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/graph/rule_pattern.hpp"

using namespace pup::graph;

TEST_CASE("RulePatternRegistry basic operations", "[rule_pattern]")
{
    auto registry = RulePatternRegistry {};

    SECTION("empty registry")
    {
        REQUIRE(registry.empty());
        REQUIRE(registry.size() == 0);
    }

    SECTION("register pattern")
    {
        registry.register_pattern(make_gcc_depfile_pattern());
        REQUIRE(!registry.empty());
        REQUIRE(registry.size() == 1);
    }
}

TEST_CASE("GCC depfile pattern", "[rule_pattern]")
{
    auto registry = RulePatternRegistry {};
    registry.register_pattern(make_gcc_depfile_pattern());

    SECTION("matches gcc compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 42,
            .command = "gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);

        auto const& rule = generated[0];
        REQUIRE(rule.inputs.size() == 1);
        REQUIRE(rule.inputs[0] == "foo.c");
        REQUIRE(rule.command == "gcc -M foo.c");
        REQUIRE(rule.action == OutputAction::InjectImplicitDeps);
        REQUIRE(rule.parent_command == 42);
        REQUIRE(rule.outputs.size() == 1);
        REQUIRE(rule.outputs[0].type == GeneratedOutput::Type::Stdout);
    }

    SECTION("matches clang compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 100,
            .command = "clang -c bar.cpp -o bar.o",
            .display = "CXX bar.o",
            .inputs = { "bar.cpp" },
            .order_only_inputs = {},
            .outputs = { "bar.o" },
            .working_dir = "src",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "clang -M bar.cpp");
    }

    SECTION("matches g++ compile command and preserves std flag")
    {
        auto cmd = CommandInfo {
            .node_id = 200,
            .command = "g++ -std=c++20 -c main.cpp -o main.o",
            .display = "CXX main.o",
            .inputs = { "main.cpp" },
            .order_only_inputs = {},
            .outputs = { "main.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "g++ -M -std=c++20 main.cpp");
    }

    SECTION("handles ccache wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 250,
            .command = "ccache gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "ccache gcc -M foo.c");
    }

    SECTION("handles distcc wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 251,
            .command = "distcc g++ -c bar.cpp -o bar.o",
            .display = "CXX bar.o",
            .inputs = { "bar.cpp" },
            .order_only_inputs = {},
            .outputs = { "bar.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "distcc g++ -M bar.cpp");
    }

    SECTION("handles cross-compiler")
    {
        auto cmd = CommandInfo {
            .node_id = 260,
            .command = "arm-linux-gnueabihf-gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "arm-linux-gnueabihf-gcc -M foo.c");
    }

    SECTION("handles absolute path to compiler")
    {
        auto cmd = CommandInfo {
            .node_id = 270,
            .command = "/usr/bin/gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "/usr/bin/gcc -M foo.c");
    }

    SECTION("skips command with -MD flag")
    {
        auto cmd = CommandInfo {
            .node_id = 300,
            .command = "gcc -MD -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -MMD flag")
    {
        auto cmd = CommandInfo {
            .node_id = 400,
            .command = "gcc -MMD -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -MF flag")
    {
        auto cmd = CommandInfo {
            .node_id = 410,
            .command = "gcc -MF deps.d -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with bare -M flag")
    {
        auto cmd = CommandInfo {
            .node_id = 420,
            .command = "gcc -M foo.c",
            .display = "DEP foo.c",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = {},
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -M at end of string")
    {
        auto cmd = CommandInfo {
            .node_id = 430,
            .command = "gcc -c foo.c -M",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("preserves -D macro definitions")
    {
        auto cmd = CommandInfo {
            .node_id = 440,
            .command = "gcc -DMYDEBUG -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "gcc -M -DMYDEBUG foo.c");
    }

    SECTION("preserves include paths")
    {
        auto cmd = CommandInfo {
            .node_id = 450,
            .command = "g++ -I../../include -I../../third_party -c foo.cpp -o foo.o",
            .display = "CXX foo.o",
            .inputs = { "foo.cpp" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = "build/src",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "g++ -M -I../../include -I../../third_party foo.cpp");
    }

    SECTION("preserves all relevant flags")
    {
        auto cmd = CommandInfo {
            .node_id = 460,
            .command = "g++ -std=c++20 -Wall -Wextra -I../include -DNDEBUG -O2 -c main.cpp -o main.o",
            .display = "CXX main.o",
            .inputs = { "main.cpp" },
            .order_only_inputs = {},
            .outputs = { "main.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // Should preserve -std, -I, -D but not -Wall, -Wextra, -O2
        REQUIRE(generated[0].command == "g++ -M -std=c++20 -I../include -DNDEBUG main.cpp");
    }

    SECTION("does not match link command")
    {
        auto cmd = CommandInfo {
            .node_id = 500,
            .command = "gcc foo.o bar.o -o app",
            .display = "LINK app",
            .inputs = { "foo.o", "bar.o" },
            .order_only_inputs = {},
            .outputs = { "app" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("does not match non-compiler command")
    {
        auto cmd = CommandInfo {
            .node_id = 600,
            .command = "ar rcs libfoo.a foo.o bar.o",
            .display = "AR libfoo.a",
            .inputs = { "foo.o", "bar.o" },
            .order_only_inputs = {},
            .outputs = { "libfoo.a" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }
}

TEST_CASE("GCC depfile pattern edge cases", "[rule_pattern]")
{
    auto registry = RulePatternRegistry {};
    registry.register_pattern(make_gcc_depfile_pattern());

    SECTION("handles multiple source files")
    {
        auto cmd = CommandInfo {
            .node_id = 700,
            .command = "gcc -c foo.c bar.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c", "bar.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "gcc -M foo.c bar.c");
    }

    SECTION("preserves -isystem flag")
    {
        auto cmd = CommandInfo {
            .node_id = 710,
            .command = "gcc -isystem/usr/local/include -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "gcc -M -isystem/usr/local/include foo.c");
    }

    SECTION("preserves -iquote flag")
    {
        auto cmd = CommandInfo {
            .node_id = 720,
            .command = "gcc -iquote../include -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "gcc -M -iquote../include foo.c");
    }

    SECTION("preserves -U undefine flag")
    {
        auto cmd = CommandInfo {
            .node_id = 730,
            .command = "gcc -DFOO -UBAR -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "gcc -M -DFOO -UBAR foo.c");
    }

    SECTION("preserves -include flag with argument")
    {
        auto cmd = CommandInfo {
            .node_id = 740,
            .command = "gcc -include config.h -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "gcc -M -include config.h foo.c");
    }

    SECTION("preserves --sysroot flag")
    {
        auto cmd = CommandInfo {
            .node_id = 750,
            .command = "gcc --sysroot=/opt/sdk -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "gcc -M --sysroot=/opt/sdk foo.c");
    }

    SECTION("preserves -D with nested quotes (mbedtls config)")
    {
        // Real pattern from spos project: -DMBEDTLS_CONFIG_FILE='"path/config.h"'
        // Shell tokenizer strips outer single quotes, preserving inner double quotes
        // Output must be re-quoted for shell execution
        auto cmd = CommandInfo {
            .node_id = 755,
            .command = R"(gcc -DMBEDTLS_CONFIG_FILE='"../include/mbedtls_config.h"' -c foo.c -o foo.o)",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // The -D flag with inner double quotes should be preserved and re-quoted
        REQUIRE(generated[0].command == R"(gcc -M '-DMBEDTLS_CONFIG_FILE="../include/mbedtls_config.h"' foo.c)");
    }

    SECTION("preserves -D with escaped double quotes")
    {
        // Pattern: -D__PFILENAME__=\"\"
        // After tokenizing: -D__PFILENAME__=""
        // Must be re-quoted for shell execution
        auto cmd = CommandInfo {
            .node_id = 756,
            .command = R"(gcc -D__PFILENAME__=\"\" -c foo.c -o foo.o)",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // The escaped quotes should be preserved and re-quoted
        REQUIRE(generated[0].command == R"(gcc -M '-D__PFILENAME__=""' foo.c)");
    }

    SECTION("handles sccache wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 760,
            .command = "sccache gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "sccache gcc -M foo.c");
    }

    SECTION("handles icecc wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 770,
            .command = "icecc g++ -c foo.cpp -o foo.o",
            .display = "CXX foo.o",
            .inputs = { "foo.cpp" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == "icecc g++ -M foo.cpp");
    }

    SECTION("generates correct display string")
    {
        auto cmd = CommandInfo {
            .node_id = 780,
            .command = "gcc -c main.c -o main.o",
            .display = "CC main.o",
            .inputs = { "main.c" },
            .order_only_inputs = {},
            .outputs = { "main.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].display == "DEP main.c");
    }

    SECTION("does not match -M embedded in path")
    {
        auto cmd = CommandInfo {
            .node_id = 790,
            .command = "gcc -c /path/to/MYMODULE/foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "/path/to/MYMODULE/foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);  // Should match (no dep flags)
    }

    SECTION("skips command with -MP flag")
    {
        auto cmd = CommandInfo {
            .node_id = 800,
            .command = "gcc -MP -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -MT flag")
    {
        auto cmd = CommandInfo {
            .node_id = 810,
            .command = "gcc -MT foo.o -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = {},
            .outputs = { "foo.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("handles C++ file extensions")
    {
        SECTION(".cc extension")
        {
            auto cmd = CommandInfo {
                .node_id = 820,
                .command = "g++ -c foo.cc -o foo.o",
                .display = "CXX foo.o",
                .inputs = { "foo.cc" },
                .order_only_inputs = {},
                .outputs = { "foo.o" },
                .working_dir = ".",
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == "g++ -M foo.cc");
        }

        SECTION(".cxx extension")
        {
            auto cmd = CommandInfo {
                .node_id = 821,
                .command = "g++ -c foo.cxx -o foo.o",
                .display = "CXX foo.o",
                .inputs = { "foo.cxx" },
                .order_only_inputs = {},
                .outputs = { "foo.o" },
                .working_dir = ".",
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == "g++ -M foo.cxx");
        }

        SECTION(".C extension")
        {
            auto cmd = CommandInfo {
                .node_id = 822,
                .command = "g++ -c foo.C -o foo.o",
                .display = "CXX foo.o",
                .inputs = { "foo.C" },
                .order_only_inputs = {},
                .outputs = { "foo.o" },
                .working_dir = ".",
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == "g++ -M foo.C");
        }

        SECTION(".c++ extension")
        {
            auto cmd = CommandInfo {
                .node_id = 823,
                .command = "g++ -c foo.c++ -o foo.o",
                .display = "CXX foo.o",
                .inputs = { "foo.c++" },
                .order_only_inputs = {},
                .outputs = { "foo.o" },
                .working_dir = ".",
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == "g++ -M foo.c++");
        }
    }
}

TEST_CASE("Generated rules inherit order-only inputs", "[rule_pattern]")
{
    auto registry = RulePatternRegistry {};
    registry.register_pattern(make_gcc_depfile_pattern());

    SECTION("DEP rule inherits order-only inputs from parent compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 900,
            .command = "gcc -c foo.c -o foo.o",
            .display = "CC foo.o",
            .inputs = { "foo.c" },
            .order_only_inputs = { "include/generated/autoconf.h", "include/generated/modules.def" },
            .outputs = { "foo.o" },
            .working_dir = "modules/test",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);

        // The generated DEP rule should inherit the order-only inputs
        REQUIRE(generated[0].order_only_inputs.size() == 2);
        REQUIRE(generated[0].order_only_inputs[0] == "include/generated/autoconf.h");
        REQUIRE(generated[0].order_only_inputs[1] == "include/generated/modules.def");
    }

    SECTION("DEP rule with empty order-only inputs")
    {
        auto cmd = CommandInfo {
            .node_id = 901,
            .command = "gcc -c bar.c -o bar.o",
            .display = "CC bar.o",
            .inputs = { "bar.c" },
            .order_only_inputs = {},
            .outputs = { "bar.o" },
            .working_dir = ".",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].order_only_inputs.empty());
    }

    SECTION("DEP rule inherits single order-only input")
    {
        auto cmd = CommandInfo {
            .node_id = 902,
            .command = "g++ -std=c++20 -c main.cpp -o main.o",
            .display = "CXX main.o",
            .inputs = { "main.cpp" },
            .order_only_inputs = { "gen-headers/config.h" },
            .outputs = { "main.o" },
            .working_dir = "src",
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].order_only_inputs.size() == 1);
        REQUIRE(generated[0].order_only_inputs[0] == "gen-headers/config.h");
    }
}

TEST_CASE("GeneratedOutput types", "[rule_pattern]")
{
    SECTION("stdout type")
    {
        auto output = GeneratedOutput {
            .type = GeneratedOutput::Type::Stdout,
            .path = {},
        };
        REQUIRE(output.type == GeneratedOutput::Type::Stdout);
    }

    SECTION("file type")
    {
        auto output = GeneratedOutput {
            .type = GeneratedOutput::Type::File,
            .path = "output.d",
        };
        REQUIRE(output.type == GeneratedOutput::Type::File);
        REQUIRE(output.path == "output.d");
    }
}
