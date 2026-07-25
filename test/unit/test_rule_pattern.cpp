// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/path_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "shell_words.hpp"

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
            .command = intern("gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);

        auto const& rule = generated[0];
        REQUIRE(rule.inputs.size() == 1);
        REQUIRE(rule.inputs[0] == intern("foo.c"));
        REQUIRE(rule.command == intern("gcc -M foo.c"));
        REQUIRE(rule.action == OutputAction::InjectImplicitDeps);
        REQUIRE(rule.parent_command == 42);
        REQUIRE(rule.outputs.size() == 1);
        REQUIRE(rule.outputs[0].type == GeneratedOutput::Type::Stdout);
    }

    SECTION("matches clang compile command")
    {
        auto cmd = CommandInfo {
            .node_id = 100,
            .command = intern("clang -c bar.cpp -o bar.o"),
            .display = intern("CXX bar.o"),
            .inputs = { intern("bar.cpp") },
            .order_only_inputs = {},
            .outputs = { path("bar.o") },
            .working_dir = intern("src"),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("clang -M bar.cpp"));
    }

    SECTION("matches g++ compile command and preserves std flag")
    {
        auto cmd = CommandInfo {
            .node_id = 200,
            .command = intern("g++ -std=c++20 -c main.cpp -o main.o"),
            .display = intern("CXX main.o"),
            .inputs = { intern("main.cpp") },
            .order_only_inputs = {},
            .outputs = { path("main.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("g++ -M -std=c++20 main.cpp"));
    }

    SECTION("handles ccache wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 250,
            .command = intern("ccache gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("ccache gcc -M foo.c"));
    }

    SECTION("handles distcc wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 251,
            .command = intern("distcc g++ -c bar.cpp -o bar.o"),
            .display = intern("CXX bar.o"),
            .inputs = { intern("bar.cpp") },
            .order_only_inputs = {},
            .outputs = { path("bar.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("distcc g++ -M bar.cpp"));
    }

    SECTION("handles cross-compiler")
    {
        auto cmd = CommandInfo {
            .node_id = 260,
            .command = intern("arm-linux-gnueabihf-gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("arm-linux-gnueabihf-gcc -M foo.c"));
    }

    SECTION("handles absolute path to compiler")
    {
        auto cmd = CommandInfo {
            .node_id = 270,
            .command = intern("/usr/bin/gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("/usr/bin/gcc -M foo.c"));
    }

    SECTION("skips command with -MD flag")
    {
        auto cmd = CommandInfo {
            .node_id = 300,
            .command = intern("gcc -MD -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -MMD flag")
    {
        auto cmd = CommandInfo {
            .node_id = 400,
            .command = intern("gcc -MMD -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -MF flag")
    {
        auto cmd = CommandInfo {
            .node_id = 410,
            .command = intern("gcc -MF deps.d -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with bare -M flag")
    {
        auto cmd = CommandInfo {
            .node_id = 420,
            .command = intern("gcc -M foo.c"),
            .display = intern("DEP foo.c"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = {},
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -M at end of string")
    {
        auto cmd = CommandInfo {
            .node_id = 430,
            .command = intern("gcc -c foo.c -M"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("preserves -D macro definitions")
    {
        auto cmd = CommandInfo {
            .node_id = 440,
            .command = intern("gcc -DMYDEBUG -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M -DMYDEBUG foo.c"));
    }

    SECTION("preserves include paths")
    {
        auto cmd = CommandInfo {
            .node_id = 450,
            .command = intern("g++ -I../../include -I../../third_party -c foo.cpp -o foo.o"),
            .display = intern("CXX foo.o"),
            .inputs = { intern("foo.cpp") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("build/src"),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("g++ -M -I../../include -I../../third_party foo.cpp"));
    }

    SECTION("preserves all relevant flags")
    {
        auto cmd = CommandInfo {
            .node_id = 460,
            .command = intern("g++ -std=c++20 -Wall -Wextra -I../include -DNDEBUG -O2 -c main.cpp -o main.o"),
            .display = intern("CXX main.o"),
            .inputs = { intern("main.cpp") },
            .order_only_inputs = {},
            .outputs = { path("main.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // Should preserve -std, -I, -D but not -Wall, -Wextra, -O2
        REQUIRE(generated[0].command == intern("g++ -M -std=c++20 -I../include -DNDEBUG main.cpp"));
    }

    SECTION("does not match link command")
    {
        auto cmd = CommandInfo {
            .node_id = 500,
            .command = intern("gcc foo.o bar.o -o app"),
            .display = intern("LINK app"),
            .inputs = { intern("foo.o"), intern("bar.o") },
            .order_only_inputs = {},
            .outputs = { path("app") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("does not match non-compiler command")
    {
        auto cmd = CommandInfo {
            .node_id = 600,
            .command = intern("ar rcs libfoo.a foo.o bar.o"),
            .display = intern("AR libfoo.a"),
            .inputs = { intern("foo.o"), intern("bar.o") },
            .order_only_inputs = {},
            .outputs = { path("libfoo.a") },
            .working_dir = intern("."),
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
            .command = intern("gcc -c foo.c bar.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c"), intern("bar.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M foo.c bar.c"));
    }

    SECTION("preserves -isystem flag")
    {
        auto cmd = CommandInfo {
            .node_id = 710,
            .command = intern("gcc -isystem/usr/local/include -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M -isystem/usr/local/include foo.c"));
    }

    SECTION("preserves -iquote flag")
    {
        auto cmd = CommandInfo {
            .node_id = 720,
            .command = intern("gcc -iquote../include -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M -iquote../include foo.c"));
    }

    SECTION("preserves -U undefine flag")
    {
        auto cmd = CommandInfo {
            .node_id = 730,
            .command = intern("gcc -DFOO -UBAR -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M -DFOO -UBAR foo.c"));
    }

    SECTION("preserves -include flag with argument")
    {
        auto cmd = CommandInfo {
            .node_id = 740,
            .command = intern("gcc -include config.h -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M -include config.h foo.c"));
    }

    SECTION("preserves --sysroot flag")
    {
        auto cmd = CommandInfo {
            .node_id = 750,
            .command = intern("gcc --sysroot=/opt/sdk -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M --sysroot=/opt/sdk foo.c"));
    }

    SECTION("preserves -D with nested quotes (mbedtls config)")
    {
        // Real pattern from spos project: -DMBEDTLS_CONFIG_FILE='"path/config.h"'
        // Shell tokenizer strips outer single quotes, preserving inner double quotes
        // Output must be re-quoted for shell execution
        auto cmd = CommandInfo {
            .node_id = 755,
            .command = intern(R"(gcc -DMBEDTLS_CONFIG_FILE='"../include/mbedtls_config.h"' -c foo.c -o foo.o)"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // The -D flag with inner double quotes should be preserved and re-quoted
        auto words = pup::test::split_for_host(pup::global_pool().get(generated[0].command));
        REQUIRE(words.has_value());
        REQUIRE(
            *words
            == std::vector<std::string> {
                "gcc",
                "-M",
                R"(-DMBEDTLS_CONFIG_FILE="../include/mbedtls_config.h")",
                "foo.c",
            }
        );
    }

    SECTION("preserves -D with escaped double quotes")
    {
        // Pattern: -D__PFILENAME__=\"\"
        // After tokenizing: -D__PFILENAME__=""
        // Must be re-quoted for shell execution
        auto cmd = CommandInfo {
            .node_id = 756,
            .command = intern(R"(gcc -D__PFILENAME__=\"\" -c foo.c -o foo.o)"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // The escaped quotes should be preserved and re-quoted
        auto words = pup::test::split_for_host(pup::global_pool().get(generated[0].command));
        REQUIRE(words.has_value());
        REQUIRE(*words == std::vector<std::string> { "gcc", "-M", R"(-D__PFILENAME__="")", "foo.c" });
    }

    SECTION("handles sccache wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 760,
            .command = intern("sccache gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("sccache gcc -M foo.c"));
    }

    SECTION("handles icecc wrapper")
    {
        auto cmd = CommandInfo {
            .node_id = 770,
            .command = intern("icecc g++ -c foo.cpp -o foo.o"),
            .display = intern("CXX foo.o"),
            .inputs = { intern("foo.cpp") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("icecc g++ -M foo.cpp"));
    }

    SECTION("generates correct display string")
    {
        auto cmd = CommandInfo {
            .node_id = 780,
            .command = intern("gcc -c main.c -o main.o"),
            .display = intern("CC main.o"),
            .inputs = { intern("main.c") },
            .order_only_inputs = {},
            .outputs = { path("main.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].display == intern("DEP main.c"));
    }

    SECTION("does not match -M embedded in path")
    {
        auto cmd = CommandInfo {
            .node_id = 790,
            .command = intern("gcc -c /path/to/MYMODULE/foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("/path/to/MYMODULE/foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);  // Should match (no dep flags)
    }

    SECTION("skips command with -MP flag")
    {
        auto cmd = CommandInfo {
            .node_id = 800,
            .command = intern("gcc -MP -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips command with -MT flag")
    {
        auto cmd = CommandInfo {
            .node_id = 810,
            .command = intern("gcc -MT foo.o -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
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
                .command = intern("g++ -c foo.cc -o foo.o"),
                .display = intern("CXX foo.o"),
                .inputs = { intern("foo.cc") },
                .order_only_inputs = {},
                .outputs = { path("foo.o") },
                .working_dir = intern("."),
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == intern("g++ -M foo.cc"));
        }

        SECTION(".cxx extension")
        {
            auto cmd = CommandInfo {
                .node_id = 821,
                .command = intern("g++ -c foo.cxx -o foo.o"),
                .display = intern("CXX foo.o"),
                .inputs = { intern("foo.cxx") },
                .order_only_inputs = {},
                .outputs = { path("foo.o") },
                .working_dir = intern("."),
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == intern("g++ -M foo.cxx"));
        }

        SECTION(".C extension")
        {
            auto cmd = CommandInfo {
                .node_id = 822,
                .command = intern("g++ -c foo.C -o foo.o"),
                .display = intern("CXX foo.o"),
                .inputs = { intern("foo.C") },
                .order_only_inputs = {},
                .outputs = { path("foo.o") },
                .working_dir = intern("."),
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == intern("g++ -M foo.C"));
        }

        SECTION(".c++ extension")
        {
            auto cmd = CommandInfo {
                .node_id = 823,
                .command = intern("g++ -c foo.c++ -o foo.o"),
                .display = intern("CXX foo.o"),
                .inputs = { intern("foo.c++") },
                .order_only_inputs = {},
                .outputs = { path("foo.o") },
                .working_dir = intern("."),
            };

            auto generated = registry.match_and_generate(cmd);
            REQUIRE(generated.size() == 1);
            REQUIRE(generated[0].command == intern("g++ -M foo.c++"));
        }
    }

    SECTION("normalizes paths with .. segments in -I flags")
    {
        // Simple case: build/dir/../../include -> include
        auto cmd = CommandInfo {
            .node_id = 830,
            .command = intern("gcc -Ibuild/dir/../../include -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // Path normalized: build/dir/../../include -> include
        REQUIRE(generated[0].command == intern("gcc -M -Iinclude foo.c"));
    }

    SECTION("normalizes complex variant build paths")
    {
        // Pattern from spos: command runs from modules/MSR/driver/
        // with -I$(TUP_VARIANT_OUTPUTDIR)/include/generated
        // which expands to -I../../../build-s1f3/modules/MSR/driver/../../../include/generated
        // The ../../../ goes up from driver/, and modules/MSR/driver/../../../ cancels out
        auto cmd = CommandInfo {
            .node_id = 831,
            .command = intern("gcc -I../../../build-s1f3/modules/MSR/driver/../../../include/generated -c driver.c -o driver.o"),
            .display = intern("CC driver.o"),
            .inputs = { intern("driver.c") },
            .order_only_inputs = {},
            .outputs = { path("driver.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        // Normalized: the driver/../../../ removes modules/MSR/driver, leaving build-s1f3/include/generated
        REQUIRE(generated[0].command == intern("gcc -M -I../../../build-s1f3/include/generated driver.c"));
    }

    SECTION("skips compound shell commands with for loops")
    {
        auto cmd = CommandInfo {
            .node_id = 840,
            .command = intern("for f in archive bfd cache; do gcc -O2 -c /src/bfd/$f.c -o out/bfd-$f.o || exit 1; done"),
            .display = intern("CC-BFD (3 files)"),
            .inputs = {},
            .order_only_inputs = {},
            .outputs = { path("bfd-archive.o"), path("bfd-bfd.o"), path("bfd-cache.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("skips cd-and-compile compound commands")
    {
        auto cmd = CommandInfo {
            .node_id = 841,
            .command = intern("cd /build && gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = {},
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.empty());
    }

    SECTION("preserves current directory reference")
    {
        auto cmd = CommandInfo {
            .node_id = 832,
            .command = intern("gcc -I. -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = {},
            .outputs = { path("foo.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M -I. foo.c"));
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
            .command = intern("gcc -c foo.c -o foo.o"),
            .display = intern("CC foo.o"),
            .inputs = { intern("foo.c") },
            .order_only_inputs = { intern("include/generated/autoconf.h"), intern("include/generated/modules.def") },
            .outputs = { path("foo.o") },
            .working_dir = intern("modules/test"),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);

        // The generated DEP rule should inherit the order-only inputs
        REQUIRE(generated[0].order_only_inputs.size() == 2);
        REQUIRE(generated[0].order_only_inputs[0] == intern("include/generated/autoconf.h"));
        REQUIRE(generated[0].order_only_inputs[1] == intern("include/generated/modules.def"));
    }

    SECTION("DEP rule with empty order-only inputs")
    {
        auto cmd = CommandInfo {
            .node_id = 901,
            .command = intern("gcc -c bar.c -o bar.o"),
            .display = intern("CC bar.o"),
            .inputs = { intern("bar.c") },
            .order_only_inputs = {},
            .outputs = { path("bar.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].order_only_inputs.empty());
    }

    SECTION("DEP rule inherits single order-only input")
    {
        auto cmd = CommandInfo {
            .node_id = 902,
            .command = intern("g++ -std=c++20 -c main.cpp -o main.o"),
            .display = intern("CXX main.o"),
            .inputs = { intern("main.cpp") },
            .order_only_inputs = { intern("gen-headers/config.h") },
            .outputs = { path("main.o") },
            .working_dir = intern("src"),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].order_only_inputs.size() == 1);
        REQUIRE(generated[0].order_only_inputs[0] == intern("gen-headers/config.h"));
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
            .path = intern("output.d"),
        };
        REQUIRE(output.type == GeneratedOutput::Type::File);
        REQUIRE(output.path == intern("output.d"));
    }
}
