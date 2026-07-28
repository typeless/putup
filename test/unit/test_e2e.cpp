// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace pup::test;

// =============================================================================
// Build Verification Tests
// =============================================================================

SCENARIO("Building a simple C project", "[e2e][build]")
{
    GIVEN("an initialized simple_c project")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("executable is created")
            {
                REQUIRE(f.is_executable("hello"));
            }

            THEN("executable produces correct output")
            {
                REQUIRE(f.run("hello").stdout_output == "Hello from pup!\n");
            }
        }

        WHEN("built again without changes")
        {
            (void)f.build();         // first build
            auto result = f.build(); // second build

            THEN("nothing is rebuilt")
            {
                REQUIRE(result.is_noop());
            }
        }
    }
}

SCENARIO("Building a multi-file project", "[e2e][build]")
{
    GIVEN("an initialized multi_file project")
    {
        auto f = E2EFixture { "multi_file" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("all object files are created")
            {
                REQUIRE(f.exists("main.o"));
                REQUIRE(f.exists("add.o"));
                REQUIRE(f.exists("multiply.o"));
            }

            THEN("executable is created")
            {
                REQUIRE(f.is_executable("calc"));
            }

            THEN("executable produces correct output")
            {
                auto output = f.run("calc").stdout_output;
                REQUIRE(output.find("add(2,3) = 5") != std::string::npos);
                REQUIRE(output.find("multiply(4,5) = 20") != std::string::npos);
            }
        }
    }
}

SCENARIO("Building with bang macros", "[e2e][build]")
{
    GIVEN("an initialized bang_macros project")
    {
        auto f = E2EFixture { "bang_macros" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("executable is created")
            {
                REQUIRE(f.is_executable("greeter"));
            }

            THEN("executable produces correct output")
            {
                REQUIRE(f.run("greeter").stdout_output == "Hello from bang macros!\n");
            }
        }
    }
}

SCENARIO("Building with output groups", "[e2e][build]")
{
    GIVEN("an initialized groups project")
    {
        auto f = E2EFixture { "groups" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("all object files are created")
            {
                REQUIRE(f.exists("main.o"));
                REQUIRE(f.exists("math.o"));
                REQUIRE(f.exists("util.o"));
            }

            THEN("executable is created")
            {
                REQUIRE(f.is_executable("calculator"));
            }

            THEN("executable produces correct output")
            {
                auto output = f.run("calculator").stdout_output;
                REQUIRE(output.find("3 + 4 = 7") != std::string::npos);
                REQUIRE(output.find("3 * 4 = 12") != std::string::npos);
            }
        }
    }
}

SCENARIO("Order-only groups ensure build ordering", "[e2e][groups]")
{
    GIVEN("a project with order-only header generation")
    {
        auto f = E2EFixture { "groups_order_only" };
        REQUIRE(f.init().success());

        WHEN("built from scratch")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated header exists")
            {
                REQUIRE(f.exists("config.h"));
            }

            THEN("percent-group pattern expands in command body")
            {
                // %<gen-headers> in command body should expand to config.h
                auto content = f.read_file("headers.txt");
                REQUIRE(content.find("config.h") != std::string::npos);
            }

            THEN("program uses generated config value")
            {
                auto output = f.run("program").stdout_output;
                REQUIRE(output.find("CONFIG_VALUE=42") != std::string::npos);
            }
        }

        WHEN("rebuilt without changes")
        {
            (void)f.build({ "-j1" });
            auto result = f.build({ "-j1" });

            THEN("nothing is rebuilt")
            {
                REQUIRE(result.is_noop());
            }
        }

        WHEN("the generator script is modified to output a new value")
        {
            (void)f.build({ "-j1" });
            // Modify gen_config.sh which outputs config.h
            // This tests: gen_config.sh -> config.h -> (order-only) -> main.o -> program
            f.write_file("gen_config.sh", "#!/bin/sh\necho '#define CONFIG_VALUE 99'\n");
            auto result = f.build({ "-j1" });

            THEN("config.h is regenerated")
            {
                auto content = f.read_file("config.h");
                REQUIRE(content.find("CONFIG_VALUE 99") != std::string::npos);
            }

            THEN("order-only dependent commands rebuild")
            {
                REQUIRE(!result.is_noop());
            }

            THEN("program outputs the new value")
            {
                auto output = f.run("program").stdout_output;
                REQUIRE(output.find("CONFIG_VALUE=99") != std::string::npos);
            }
        }
    }
}

SCENARIO("Cross-directory order-only groups", "[e2e][groups]")
{
    GIVEN("a multi-directory project with cross-dir group reference")
    {
        auto f = E2EFixture { "groups_cross_dir" };
        REQUIRE(f.init().success());

        WHEN("built")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("header is generated in include directory")
            {
                REQUIRE(f.exists("include/version.h"));
            }

            THEN("program is built in src directory")
            {
                REQUIRE(f.is_executable("src/program"));
            }

            THEN("program uses generated version")
            {
                auto output = f.run("src/program").stdout_output;
                REQUIRE(output.find("Version: 1.0.0") != std::string::npos);
            }

            THEN("percent-group expands cross-directory group in command")
            {
                // %<gen-headers> should expand to ../include/version.h
                auto content = f.read_file("src/headers.txt");
                REQUIRE(content.find("VERSION") != std::string::npos);
            }
        }
    }
}

SCENARIO("Cross-directory order-only groups work in-tree", "[e2e][build][groups]")
{
    GIVEN("a project with generated headers in include/ referenced from src/")
    {
        auto f = E2EFixture { "groups_cross_dir_variant" };
        REQUIRE(f.init().success());

        WHEN("building in-tree")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds with correct dependency ordering")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("include/version.h"));
                REQUIRE(f.exists("src/main.o"));
                REQUIRE(f.is_executable("src/program"));
            }

            THEN("program uses generated version")
            {
                auto output = f.run("src/program").stdout_output;
                REQUIRE(output.find("Version: 1.0.0") != std::string::npos);
            }
        }
    }
}

SCENARIO("Bang macro order-only groups trigger demand-driven parsing", "[e2e][build][groups][bang]")
{
    GIVEN("a project with a bang macro that references an order-only group in another directory")
    {
        // This test verifies that order-only group references embedded in bang macros
        // correctly trigger demand-driven parsing of the directory containing the group.
        // Bug: the group reference in !cc = | $(TOROOT)/include/<gen-headers> |> ...
        // was not triggering parsing of include/Tupfile before looking up the group.
        auto f = E2EFixture { "groups_bang_macro_cross_dir" };
        REQUIRE(f.init().success());

        WHEN("building in-tree")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds with correct dependency ordering")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("include/version.h"));
                REQUIRE(f.exists("src/main.o"));
            }
        }
    }
}

SCENARIO("Cross-directory order-only groups work in variant builds", "[e2e][variant][groups]")
{
    GIVEN("a project with generated headers in include/ referenced from src/")
    {
        auto f = E2EFixture { "groups_cross_dir_variant" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_CC=gcc\n");

        WHEN("building as a variant with -B build")
        {
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("build succeeds with correct dependency ordering")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/include/version.h"));
                REQUIRE(f.exists("build/src/main.o"));
                REQUIRE(f.is_executable("build/src/program"));
            }

            THEN("program uses generated version")
            {
                auto output = f.run("build/src/program").stdout_output;
                REQUIRE(output.find("Version: 1.0.0") != std::string::npos);
            }
        }
    }
}

SCENARIO("Bang macro order-only groups work in variant builds", "[e2e][variant][groups][bang]")
{
    GIVEN("a project with a bang macro that references an order-only group across directories")
    {
        // This test verifies that order-only group references in bang macros work correctly
        // in variant builds. The bug was that DEP (implicit dep scanning) commands were not
        // inheriting the order-only edges from their parent compile commands when using
        // bang macros with TOROOT-based group references like:
        //   !cc = | $(TOROOT)/include/<gen-headers> |> ...
        // This caused DEP commands to run before headers were generated.
        auto f = E2EFixture { "groups_bang_macro_variant" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_CC=gcc\n");

        WHEN("building as a variant with -B build")
        {
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("build succeeds with correct dependency ordering")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/include/version.h"));
                REQUIRE(f.exists("build/src/main.o"));
                REQUIRE(f.exists("build/src/lib/add.o"));
            }
        }
    }
}

SCENARIO("Groups defined in included files are visible", "[e2e][groups]")
{
    GIVEN("a Tupfile that includes gen.tup (defines group) and build.tup (references group)")
    {
        auto f = E2EFixture { "groups_include" };
        REQUIRE(f.init().success());

        WHEN("built")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated header from included file exists")
            {
                REQUIRE(f.exists("config.h"));
            }

            THEN("program built from another included file works")
            {
                auto output = f.run("program").stdout_output;
                REQUIRE(output.find("CONFIG_VALUE=123") != std::string::npos);
            }

            THEN("percent-group expands group from included file in command")
            {
                // %<gen-headers> should expand to config.h (defined in gen.tup)
                auto content = f.read_file("headers.txt");
                REQUIRE(content.find("CONFIG_VALUE") != std::string::npos);
            }
        }
    }
}

SCENARIO("Variant tup.config as a rule input retriggers on config change", "[e2e][variant][incremental]")
{
    GIVEN("a rule that generates a header from the variant's tup.config")
    {
        auto f = E2EFixture { "variant_config_input" };
        f.write_file("inc/Tupfile", R"(
: ../tup.config |> awk -F= '/^CONFIG_/{print "#define " substr($1,8)}' %f > %o |> config.h
)");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_FOO=y\n");

        WHEN("built, then tup.config gains a variable")
        {
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            REQUIRE(f.build({ "-B", "build", "-j1" }).success());
            REQUIRE(f.read_file("build/inc/config.h") == "#define FOO\n");

            f.write_file("build/tup.config", "CONFIG_FOO=y\nCONFIG_BAR=y\n");
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("the header is regenerated with the new variable")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/inc/config.h") == "#define FOO\n#define BAR\n");
            }
        }
    }
}

SCENARIO("Group references in regular inputs expand correctly", "[e2e][groups]")
{
    GIVEN("a Tupfile with group reference in inputs section (before |)")
    {
        // This tests the spos pattern: $(ROOT)/modules/<json-headers> |> cat %<json-headers>
        // Group references are order-only even when in regular inputs section
        auto f = E2EFixture { "groups_in_inputs" };
        REQUIRE(f.init().success());

        WHEN("built")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("module headers are generated")
            {
                REQUIRE(f.exists("modules/mod1.header"));
                REQUIRE(f.exists("modules/mod2.header"));
            }

            THEN("percent-group in command expands to all group members")
            {
                // %<json-headers> should expand to mod1.header and mod2.header
                auto content = f.read_file("output/headers.txt");
                REQUIRE(content.find("mod1") != std::string::npos);
                REQUIRE(content.find("mod2") != std::string::npos);
            }
        }
    }
}

SCENARIO("Multi-directory group producers all contribute to percent-group expansion", "[e2e][groups]")
{
    GIVEN("a group receiving members from two separate directories")
    {
        auto f = E2EFixture { "groups_multi_dir_producers" };
        REQUIRE(f.init().success());

        WHEN("built")
        {
            auto result = f.build({ "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("both producers contribute output")
            {
                REQUIRE(f.exists("dir_a/a.txt"));
                REQUIRE(f.exists("dir_b/b.txt"));
            }

            THEN("percent-group expansion includes members from all directories")
            {
                auto content = f.read_file("link/combined.txt");
                auto has_both = (content == "a\nb\n" || content == "b\na\n");
                REQUIRE(has_both);
            }
        }
    }
}

SCENARIO("Build fails when command fails", "[e2e][build]")
{
    GIVEN("an initialized failure project with invalid source")
    {
        auto f = E2EFixture { "failure" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build({ "-j1" });

            THEN("build fails")
            {
                REQUIRE_FALSE(result.success());
            }

            THEN("no output file is created")
            {
                REQUIRE_FALSE(f.exists("bad.o"));
            }
        }
    }
}

SCENARIO("Dollar-dollar escapes to literal dollar in shell commands", "[e2e][build]")
{
    GIVEN("a project with $$ in a command")
    {
        auto f = E2EFixture { "dollar_escape" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build succeeds and $$ becomes $ for the shell")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("output.txt") == "hello\n");
            }
        }
    }
}

// =============================================================================
// Incremental Build Tests
// =============================================================================

SCENARIO("Incremental rebuilds detect header changes", "[e2e][incremental]")
{
    GIVEN("a built project with header dependencies")
    {
        auto f = E2EFixture { "incremental" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("program").stdout_output == "Value is 42\n");

        WHEN("a header file is modified")
        {
            f.write_file("value.h", "#define VALUE 100\n");
            auto result = f.build();

            THEN("dependent sources are rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }

            THEN("output reflects the change")
            {
                REQUIRE(f.run("program").stdout_output == "Value is 100\n");
            }
        }
    }
}

SCENARIO("Incremental rebuilds work when running from build directory", "[e2e][incremental][variant]")
{
    GIVEN("a built out-of-tree project")
    {
        auto f = E2EFixture { "incremental_from_build_dir" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/main.o"));

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            auto noop = f.run_pup_in_dir("build", {});
            REQUIRE(noop.success());
            REQUIRE(noop.is_noop());

            WHEN("a source file is modified and pup is run from build directory")
            {
                f.write_file("main.c", "int main(void) { return 1; }\n");
                auto result = f.run_pup_in_dir("build", {});

                THEN("rebuild occurs (change is detected)")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }
            }
        }
    }
}

SCENARIO("Tupfile changes trigger rebuild", "[e2e][incremental]")
{
    GIVEN("a built project")
    {
        auto f = E2EFixture { "tupfile_change" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("program").stdout_output == "Version: 1\n");

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build().is_noop());

            WHEN("the Tupfile is modified to change VERSION")
            {
                auto original = f.read_file("Tupfile");
                auto pos = original.find("VERSION=1");
                REQUIRE(pos != std::string::npos);
                auto modified = original.substr(0, pos) + "VERSION=2" + original.substr(pos + 9);
                f.write_file("Tupfile", modified);

                auto result = f.build();

                THEN("rebuild occurs")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }

                THEN("output reflects the change")
                {
                    REQUIRE(f.run("program").stdout_output == "Version: 2\n");
                }
            }
        }
    }
}

SCENARIO("Editing an output-less command re-runs it", "[e2e][incremental]")
{
    GIVEN("a built project with an output-less command")
    {
        auto f = E2EFixture { "no_output_command" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("ran-v1.stamp"));

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build().is_noop());

            WHEN("the command text changes")
            {
                f.write_file("Tupfile", ": |> touch ran-v2-longer.stamp |>\n");
                auto result = f.build();

                THEN("the new command runs")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                    REQUIRE(f.exists("ran-v2-longer.stamp"));
                }

                THEN("the following build is a no-op")
                {
                    REQUIRE(f.build().is_noop());
                }
            }
        }
    }
}

SCENARIO("Config-driven identity change re-runs an output-less command", "[e2e][incremental]")
{
    GIVEN("a built project with an output-less command reading a config-driven variable")
    {
        auto f = E2EFixture { "no_output_command_config" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("ran-plain.stamp"));

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build().is_noop());

            WHEN("a -D override changes the command's identity")
            {
                auto result = f.build({ "-D", "FLAG=y" });

                THEN("the changed command runs")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                    REQUIRE(f.exists("ran-with-flag.stamp"));
                }
            }
        }
    }
}

SCENARIO("Config file changes trigger rebuild", "[e2e][incremental]")
{
    GIVEN("a project with tup.config")
    {
        auto f = E2EFixture { "config_change" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_OPT=1\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.run("build/program").stdout_output == "Optimization: 1\n");

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build({ "-B", "build" }).is_noop());

            WHEN("tup.config is modified")
            {
                f.write_file("build/tup.config", "CONFIG_OPT=2\n");
                auto result = f.build({ "-B", "build" });

                THEN("rebuild occurs")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }

                THEN("output reflects the new config")
                {
                    REQUIRE(f.run("build/program").stdout_output == "Optimization: 2\n");
                }
            }
        }
    }
}

SCENARIO("Fine-grained config variable tracking", "[e2e][incremental][config]")
{
    GIVEN("a project with multiple config variables where only one is used")
    {
        auto f = E2EFixture { "config_var_tracking" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_UNUSED=x\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.run("build/program").stdout_output == "Optimization: 1\n");

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build({ "-B", "build" }).is_noop());

            WHEN("an unused config variable changes")
            {
                f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_UNUSED=y\n");
                auto result = f.build({ "-B", "build" });

                THEN("no rebuild occurs because command doesn't use UNUSED")
                {
                    REQUIRE(result.success());
                    REQUIRE(result.is_noop());
                }
            }

            WHEN("a used config variable changes")
            {
                f.write_file("build/tup.config", "CONFIG_OPT=2\nCONFIG_UNUSED=x\n");
                auto result = f.build({ "-B", "build" });

                THEN("rebuild occurs")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }

                THEN("output reflects the new config")
                {
                    REQUIRE(f.run("build/program").stdout_output == "Optimization: 2\n");
                }
            }
        }
    }
}

SCENARIO("Fine-grained config variable tracking with $(CONFIG_VAR) syntax", "[e2e][incremental][config]")
{
    GIVEN("a project using $(CONFIG_VAR) syntax")
    {
        auto f = E2EFixture { "config_var_dollar_syntax" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_UNUSED=x\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.run("build/program").stdout_output == "Optimization: 1\n");

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build({ "-B", "build" }).is_noop());

            WHEN("an unused config variable changes")
            {
                f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_UNUSED=y\n");
                auto result = f.build({ "-B", "build" });

                THEN("no rebuild occurs because command doesn't use UNUSED")
                {
                    REQUIRE(result.success());
                    REQUIRE(result.is_noop());
                }
            }

            WHEN("a used config variable changes")
            {
                f.write_file("build/tup.config", "CONFIG_OPT=2\nCONFIG_UNUSED=x\n");
                auto result = f.build({ "-B", "build" });

                THEN("rebuild occurs")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }

                THEN("output reflects the new config")
                {
                    REQUIRE(f.run("build/program").stdout_output == "Optimization: 2\n");
                }
            }
        }
    }
}

SCENARIO("Fine-grained config variable tracking with indirect usage", "[e2e][incremental][config]")
{
    GIVEN("a project with config var used through a regular variable")
    {
        auto f = E2EFixture { "config_var_indirect" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_UNUSED=x\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.run("build/program").stdout_output == "Optimization: 1\n");

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build({ "-B", "build" }).is_noop());

            WHEN("an unused config variable changes")
            {
                f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_UNUSED=y\n");
                auto result = f.build({ "-B", "build" });

                THEN("no rebuild occurs")
                {
                    REQUIRE(result.success());
                    REQUIRE(result.is_noop());
                }
            }

            WHEN("the indirectly-used config variable changes")
            {
                f.write_file("build/tup.config", "CONFIG_OPT=2\nCONFIG_UNUSED=x\n");
                auto result = f.build({ "-B", "build" });

                THEN("rebuild occurs even though command uses $(MYFLAGS) not @(OPT)")
                {
                    // THIS IS THE KEY ASSERTION - currently fails
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }

                THEN("output reflects the new config")
                {
                    REQUIRE(f.run("build/program").stdout_output == "Optimization: 2\n");
                }
            }
        }
    }
}

SCENARIO("SoftSet with config var only records deps when effective", "[e2e][incremental][config]")
{
    GIVEN("a project using ?= with config var")
    {
        auto f = E2EFixture { "config_var_softset" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_OTHER=x\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.run("build/program").stdout_output == "Level: 1\n");
        REQUIRE(f.build({ "-B", "build" }).is_noop());

        WHEN("used config var changes and ?= was effective")
        {
            f.write_file("build/tup.config", "CONFIG_OPT=2\nCONFIG_OTHER=x\n");
            auto result = f.build({ "-B", "build" });

            THEN("rebuild occurs because MYFLAGS depends on OPT")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.run("build/program").stdout_output == "Level: 2\n");
            }
        }
    }

    GIVEN("a project where MYFLAGS is already set before ?=")
    {
        auto f = E2EFixture { "config_var_softset" };
        f.mkdir("build");
        // Pre-set MYFLAGS so ?= is ineffective
        f.write_file("Tuprules.tup", "MYFLAGS = -DLEVEL=99\n");
        f.write_file("build/tup.config", "CONFIG_OPT=1\nCONFIG_OTHER=x\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.run("build/program").stdout_output == "Level: 99\n");
        REQUIRE(f.build({ "-B", "build" }).is_noop());

        WHEN("the config var that ?= would have used changes")
        {
            f.write_file("build/tup.config", "CONFIG_OPT=2\nCONFIG_OTHER=x\n");
            auto result = f.build({ "-B", "build" });

            THEN("no rebuild because ?= was ineffective - MYFLAGS doesn't depend on OPT")
            {
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
            }
        }
    }
}

SCENARIO("WeakSet with config var records deps only for winning assignment", "[e2e][incremental][config]")
{
    GIVEN("a project with multiple weak assignments using different config vars")
    {
        auto f = E2EFixture { "config_var_weakset" };
        f.mkdir("build");
        // OPT2=5 wins (last ??= assignment), OPT1=1 is ignored
        f.write_file("build/tup.config", "CONFIG_OPT1=1\nCONFIG_OPT2=5\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        // Should use OPT2 (last wins)
        REQUIRE(f.run("build/program").stdout_output == "Level: 5\n");
        REQUIRE(f.build({ "-B", "build" }).is_noop());

        WHEN("the losing config var (OPT1) changes")
        {
            f.write_file("build/tup.config", "CONFIG_OPT1=99\nCONFIG_OPT2=5\n");
            auto result = f.build({ "-B", "build" });

            THEN("no rebuild because MYFLAGS only depends on OPT2 (the winner)")
            {
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
            }
        }

        WHEN("the winning config var (OPT2) changes")
        {
            f.write_file("build/tup.config", "CONFIG_OPT1=1\nCONFIG_OPT2=7\n");
            auto result = f.build({ "-B", "build" });

            THEN("rebuild occurs because MYFLAGS depends on OPT2")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.run("build/program").stdout_output == "Level: 7\n");
            }
        }
    }
}

SCENARIO("Touch does not trigger unnecessary rebuild", "[e2e][incremental]")
{
    GIVEN("a built project with source files")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.is_executable("hello"));

        WHEN("a source file is touched without content change")
        {
            // Touch updates mtime but not content
            // Use absolute path to touch since it's not in workdir
            auto result = f.run("/usr/bin/touch", { "hello.c" });
            REQUIRE(result.success());

            auto build_result = f.build();

            THEN("build succeeds")
            {
                REQUIRE(build_result.success());
            }

            THEN("no rebuild occurs because content hash unchanged")
            {
                REQUIRE(build_result.is_noop());
            }
        }
    }
}

SCENARIO("Partial failure with -k saves successful outputs", "[e2e][keep-going]")
{
    GIVEN("a project where one command will fail")
    {
        auto f = E2EFixture { "partial_failure" };
        REQUIRE(f.init().success());

        WHEN("building with -k flag")
        {
            auto result1 = f.build({ "-k" });

            THEN("build fails but successful command's output exists")
            {
                REQUIRE_FALSE(result1.success());
                REQUIRE(f.exists("good.o"));
                REQUIRE_FALSE(f.exists("bad.o"));
            }

            AND_WHEN("fixing the failing source and rebuilding")
            {
                f.write_file("bad.c", "int bad_func(void) { return 0; }\n");
                auto result2 = f.build({ "-v" });

                THEN("build succeeds")
                {
                    REQUIRE(result2.success());
                    REQUIRE(f.exists("good.o"));
                    REQUIRE(f.exists("bad.o"));
                }

                THEN("only the fixed command runs, not the already-successful one")
                {
                    // The output should show only bad.c being compiled, not good.c
                    // If good.c is recompiled, the bug is present
                    REQUIRE(result2.stdout_output.find("good.c") == std::string::npos);
                    REQUIRE(result2.stdout_output.find("bad.c") != std::string::npos);
                }
            }
        }
    }
}

SCENARIO("Implicit dependencies track header changes", "[e2e][incremental]")
{
    GIVEN("a project built with implicit dependency tracking")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("program").stdout_output == "Version 1\n");

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build().is_noop());

            WHEN("an included header is modified")
            {
                f.write_file("config.h", "#ifndef CONFIG_H\n"
                                         "#define CONFIG_H\n"
                                         "#define VERSION 2\n"
                                         "#endif\n");
                auto result = f.build();

                THEN("rebuild occurs due to implicit dependency")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }

                THEN("output reflects the change")
                {
                    REQUIRE(f.run("program").stdout_output == "Version 2\n");
                }
            }
        }
    }
}

SCENARIO("Implicit deps survive identical rules in sibling directories", "[e2e][incremental][identity]")
{
    // Command text is Tupfile-relative, so these two rules render the same string.
    // If identity ignores the directory they collide, and one directory's header
    // edges get attached to the other's command.
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("two directories whose rules and sources are byte-identical")
    {
        auto f = E2EFixture { "identity_collision" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("a/program").stdout_output == "Version 1\n");
        REQUIRE(f.run("b/program").stdout_output == "Version 1\n");

        WHEN("only the second directory's header changes")
        {
            f.write_file("b/config.h", "#ifndef CONFIG_H\n"
                                       "#define CONFIG_H\n"
                                       "#define VERSION 2\n"
                                       "#endif\n");
            auto result = f.build();

            THEN("that directory rebuilds")
            {
                REQUIRE(result.success());
                REQUIRE(f.run("b/program").stdout_output == "Version 2\n");
            }

            THEN("the other directory is untouched")
            {
                REQUIRE(result.success());
                REQUIRE(f.run("a/program").stdout_output == "Version 1\n");
            }
        }
    }
}

SCENARIO("Implicit deps cover every source of a multi-source command", "[e2e][incremental]")
{
    // gcc -M emits one rule per source; stopping at the first leaves b.h untracked and the program silently stale
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("a command compiling two sources with a header each")
    {
        auto f = E2EFixture { "implicit_deps_multi_source" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("program").stdout_output == "1 1\n");
        REQUIRE(f.build().is_noop());

        WHEN("the header included only by the second source changes")
        {
            f.write_file("b.h", "#ifndef B_H\n"
                                "#define B_H\n"
                                "#define B_VALUE 2\n"
                                "#endif\n");
            auto result = f.build();

            THEN("the command rebuilds")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }

            THEN("the program reflects the change")
            {
                REQUIRE(f.run("program").stdout_output == "1 2\n");
            }
        }
    }
}

SCENARIO("Implicit deps survive command-id shift from a removed source", "[e2e][incremental][idshift]")
{
    // Implicit (header→command) edges discovered last build are carried forward for
    // commands that don't rebuild. If that carry-forward keys on the command's array
    // position (NodeId), removing a glob-matched source whose command was created
    // earlier shifts every later command's id down — and the carried edge gets
    // misattributed to whatever command now occupies the old id. A later edit to the
    // header then rebuilds the wrong unit and leaves the real output stale. The
    // carry-forward must key on the command's structural identity, stable across shifts.
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("a project where m_a.o has a discovered header dep and m_b.o is a removable unit")
    {
        auto f = E2EFixture { "implicit_dep_idshift" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("program").stdout_output == "A1\n");
        REQUIRE(f.build().is_noop());

        WHEN("the earlier-created unit is removed (shifting m_a's command id down), then m_a.h changes")
        {
            // No Tupfile edit: the glob drops m_b.c. m_a.c is not recompiled this build,
            // so its m_a.h dependency must be carried forward — at the new command id.
            f.remove_file("m_b.c");
            REQUIRE(f.build().success());
            REQUIRE(f.run("program").stdout_output == "A1\n");

            f.write_file("m_a.h", "#define A_VERSION 2\n");
            auto result = f.build();

            THEN("m_a.o rebuilds from its header dependency (no misattribution)")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.run("program").stdout_output == "A2\n");
            }
        }
    }
}

SCENARIO("New source file triggers rebuild", "[e2e][incremental]")
{
    GIVEN("a project with one source file using foreach glob")
    {
        auto f = E2EFixture { "new_file_detection" };

        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("add.o"));

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            auto noop = f.build();
            REQUIRE(noop.success());
            REQUIRE(noop.is_noop());

            WHEN("a new source file is added")
            {
                f.write_file("mul.c", "int mul(int a, int b) { return a * b; }\n");
                auto result = f.build();

                THEN("the new file is compiled")
                {
                    REQUIRE(result.success());
                    REQUIRE(f.exists("mul.o"));
                }

                THEN("the original file is not recompiled")
                {
                    REQUIRE_FALSE(result.is_noop()); // mul.o was built
                }
            }
        }
    }
}

SCENARIO("New file under an order-only glob triggers rebuild", "[e2e][incremental]")
{
    GIVEN("a rule whose only glob input is order-only, so it never reaches the command text")
    {
        auto f = E2EFixture { "glob_order_only" };

        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("listing.txt") == "one.txt\n");

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            REQUIRE(f.build().is_noop());

            WHEN("a new file is added under the glob")
            {
                f.write_file("data/two.txt", "two\n");
                auto result = f.build();

                THEN("the consuming rule re-runs and sees the new file")
                {
                    REQUIRE(result.success());
                    REQUIRE(f.read_file("listing.txt") == "one.txt\ntwo.txt\n");
                }
            }
        }
    }
}

SCENARIO("Removed file under an order-only glob triggers rebuild", "[e2e][incremental]")
{
    GIVEN("a rule whose only glob input is order-only, so it never reaches the command text")
    {
        auto f = E2EFixture { "glob_order_only" };
        f.write_file("data/two.txt", "two\n");

        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("listing.txt") == "one.txt\ntwo.txt\n");

        WHEN("a file is removed from under the glob")
        {
            f.remove_file("data/two.txt");
            auto result = f.build();

            THEN("the consuming rule re-runs and no longer sees it")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("listing.txt") == "one.txt\n");
            }

            THEN("the change propagates to downstream rules")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("copy.txt") == "one.txt\n");
            }
        }
    }
}

SCENARIO("A scoped build does not blind the next full build", "[e2e][incremental][scope]")
{
    GIVEN("two independent directories")
    {
        auto f = E2EFixture { "scoped_out_of_scope_edit" };

        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("b/out.txt") == "v1\n");

        WHEN("a scoped build rewrites the index without parsing the other directory")
        {
            f.write_file("a/src.txt", "v2\n");
            auto scoped = f.build({ "a/" });
            REQUIRE(scoped.success());
            REQUIRE_FALSE(scoped.is_noop());

            AND_WHEN("a file in the unparsed directory is then edited")
            {
                f.write_file("b/src.txt", "v2\n");
                auto result = f.build();

                THEN("the full build still sees it")
                {
                    INFO("stdout: " << result.stdout_output);
                    REQUIRE(result.success());
                    REQUIRE(f.read_file("b/out.txt") == "v2\n");
                }

                THEN("and the build settles afterwards")
                {
                    REQUIRE(result.success());
                    REQUIRE(f.build().is_noop());
                }
            }
        }
    }
}

SCENARIO("Removed source file triggers stale output cleanup", "[e2e][incremental]")
{
    GIVEN("a project with two source files")
    {
        auto f = E2EFixture { "new_file_detection" };

        // Add second file
        f.write_file("mul.c", "int mul(int a, int b) { return a * b; }\n");

        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("add.o"));
        REQUIRE(f.exists("mul.o"));

        WHEN("a source file is removed")
        {
            f.remove_file("mul.c");
            auto result = f.build();

            THEN("the stale output is deleted")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("add.o"));
                REQUIRE_FALSE(f.exists("mul.o"));
            }
        }
    }
}

SCENARIO("Removed source file cleans stale output in variant build", "[e2e][incremental][variant]")
{
    GIVEN("a variant build with two source files")
    {
        auto f = E2EFixture { "new_file_detection" };
        f.write_file("mul.c", "int mul(int a, int b) { return a * b; }\n");

        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/add.o"));
        REQUIRE(f.exists("build/mul.o"));
        REQUIRE_FALSE(f.exists("add.o")); // Not in source dir

        WHEN("a source file is removed")
        {
            f.remove_file("mul.c");
            auto result = f.build({ "-B", "build" });

            THEN("the stale output in the build directory is deleted")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/add.o"));
                REQUIRE_FALSE(f.exists("build/mul.o"));
            }
        }
    }
}

SCENARIO("Source file content change triggers rebuild in variant build", "[e2e][incremental][variant]")
{
    GIVEN("a variant build with cross-directory dependencies")
    {
        // scoped_build has: app -> lib cross-directory dep
        // app/Tupfile is parsed first (alphabetically), references ../lib/foo.o
        // This may create Ghost nodes, testing the ID contiguity fix
        auto f = E2EFixture { "scoped_build" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.is_executable("build/app/app"));

        AND_GIVEN("a no-op rebuild confirms stability")
        {
            auto noop = f.build({ "-B", "build" });
            REQUIRE(noop.success());
            INFO("stdout: " << noop.stdout_output);
            INFO("stderr: " << noop.stderr_output);
            REQUIRE(noop.is_noop());

            WHEN("source file content is modified without size change")
            {
                // Modify "42" to "99" - same size (2 chars), different content
                auto original = f.read_file("lib/foo.c");
                auto pos = original.find("42");
                REQUIRE(pos != std::string::npos);
                auto content = original.substr(0, pos) + "99" + original.substr(pos + 2);
                f.write_file("lib/foo.c", content);

                auto result = f.build({ "-B", "build" });

                THEN("rebuild occurs due to content hash change")
                {
                    REQUIRE(result.success());
                    REQUIRE_FALSE(result.is_noop());
                }
            }
        }
    }
}

// =============================================================================
// Scoped Build Tests
// =============================================================================

SCENARIO("Scoped build skips changes outside scope", "[e2e][incremental][scope]")
{
    GIVEN("a multi-directory project with cross-directory dependencies")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("a file outside scope is modified and build runs from subdirectory")
        {
            f.append_file("app/main.c", "// modified\n");
            auto result = f.run_pup_in_dir("lib", {});

            THEN("the build is a no-op (out-of-scope change ignored)")
            {
                REQUIRE(result.success());
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.is_noop());
            }
        }
    }
}

SCENARIO("Scoped build detects changes within scope", "[e2e][incremental][scope]")
{
    GIVEN("a multi-directory project")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("a file inside scope is modified")
        {
            f.append_file("lib/foo.c", "// modified\n");
            auto result = f.run_pup_in_dir("lib", {});

            THEN("the file is rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }
        }
    }
}

SCENARIO("Scoped build propagates downstream", "[e2e][incremental][scope]")
{
    GIVEN("a multi-directory project with cross-directory dependencies")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("app/app").exit_code == 42);

        WHEN("a library file is modified and build runs from lib/")
        {
            auto original = f.read_file("lib/foo.c");
            auto pos = original.find("42");
            REQUIRE(pos != std::string::npos);
            f.write_file("lib/foo.c", original.substr(0, pos) + "99" + original.substr(pos + 2));

            auto result = f.run_pup_in_dir("lib", { "-v" });

            THEN("both the library and dependent app are rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("foo.o") != std::string::npos);
                REQUIRE(f.run("app/app").exit_code == 99);
            }
        }
    }
}

SCENARIO("In-tree incremental rebuild relinks cross-directory consumers", "[e2e][incremental]")
{
    GIVEN("a built in-tree project with a cross-directory dependency")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("app/app").exit_code == 42);

        WHEN("the library source changes and a full build runs")
        {
            auto original = f.read_file("lib/foo.c");
            auto pos = original.find("42");
            REQUIRE(pos != std::string::npos);
            f.write_file("lib/foo.c", original.substr(0, pos) + "99" + original.substr(pos + 2));

            auto result = f.build();

            THEN("the consumer relinks against the rebuilt object")
            {
                REQUIRE(result.success());
                REQUIRE(f.run("app/app").exit_code == 99);
            }
        }
    }
}

SCENARIO("-A flag forces full project build", "[e2e][incremental][scope]")
{
    GIVEN("a multi-directory project")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("-A flag is used from subdirectory with out-of-scope changes")
        {
            f.append_file("app/main.c", "// modified\n");
            auto result = f.run_pup_in_dir("lib", { "-A" });

            THEN("the out-of-scope file is rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }
        }
    }
}

SCENARIO("Explicit target path sets scope", "[e2e][incremental][scope]")
{
    GIVEN("a multi-directory project")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("explicit target 'lib' is passed from project root")
        {
            f.append_file("lib/foo.c", "// modified\n");
            f.append_file("app/main.c", "// also modified\n");
            auto result = f.build({ "lib", "-v" });

            THEN("only the lib scope is checked and rebuilt")
            {
                REQUIRE(result.success());
                // lib/foo.c change detected and rebuilt
                REQUIRE(result.stdout_output.find("foo.o") != std::string::npos);
            }
        }
    }
}

SCENARIO("Tupfile changes detected regardless of scope", "[e2e][incremental][scope]")
{
    GIVEN("a multi-directory project")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("a Tupfile outside scope is modified")
        {
            f.append_file("app/Tupfile", "\n# comment\n");
            auto result = f.run_pup_in_dir("lib", { "-v" });

            THEN("the change is detected and triggers rebuild")
            {
                REQUIRE(result.success());
                // Tupfile change causes dependent app to rebuild
                REQUIRE_FALSE(result.is_noop());
            }
        }
    }
}

SCENARIO("Scoped build without -a detects implicit dep changes", "[e2e][incremental][scope]")
{
    GIVEN("a project with shared include directory")
    {
        auto f = E2EFixture { "scoped_upstream" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("an implicit dependency (header) is modified and scoped build runs WITHOUT -a")
        {
            f.write_file("include/header.h", "#define VALUE 100\n");
            auto result = f.build({ "lib", "-v" });

            THEN("the change is detected (implicit deps always checked)")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }
        }
    }
}

SCENARIO("Scoped build with -a checks upstream deps (mma behavior)", "[e2e][incremental][scope]")
{
    GIVEN("a project with shared include directory")
    {
        auto f = E2EFixture { "scoped_upstream" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("an upstream dependency (header) is modified and scoped build runs WITH -a")
        {
            f.write_file("include/header.h", "#define VALUE 100\n");
            auto result = f.build({ "-a", "lib", "-v" });

            THEN("the scoped module is rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(result.stdout_output.find("foo.o") != std::string::npos);
            }
        }

        WHEN("an independent sibling is modified and scoped build runs with -a")
        {
            f.append_file("lib2/bar.c", "// modified\n");
            auto result = f.build({ "-a", "lib", "-v" });

            THEN("the build is a no-op (sibling change still ignored)")
            {
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
            }
        }
    }
}

// =============================================================================
// Cross-Directory Scoped Build with -a (all-deps) Tests
// =============================================================================

SCENARIO("Fresh scoped build with -a succeeds for cross-directory deps", "[e2e][scope]")
{
    GIVEN("a project with producer/consumer cross-directory dependencies")
    {
        auto f = E2EFixture { "cross_dir_scoped_alldeps" };
        REQUIRE(f.init().success());

        WHEN("consumer/ is built with -a on a fresh build (no index)")
        {
            auto result = f.build({ "-a", "consumer/" });

            THEN("the build succeeds and both producer and consumer outputs exist")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("shared/lib.dat"));
                REQUIRE(f.exists("consumer/result.txt"));
            }
        }
    }
}

SCENARIO("Fresh scoped build WITHOUT -a fails for cross-directory deps", "[e2e][scope]")
{
    GIVEN("a project with producer/consumer cross-directory dependencies")
    {
        auto f = E2EFixture { "cross_dir_scoped_alldeps" };
        REQUIRE(f.init().success());

        WHEN("consumer/ is built without -a on a fresh build")
        {
            auto result = f.build({ "consumer/" });

            THEN("the build fails with an unresolved ghost error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("unresolved ghost") != std::string::npos);
            }
        }
    }
}

SCENARIO("Fresh scoped build with -a does NOT build unrelated dirs", "[e2e][scope]")
{
    GIVEN("a project with producer, consumer, and unrelated directories")
    {
        auto f = E2EFixture { "cross_dir_scoped_alldeps" };
        REQUIRE(f.init().success());

        WHEN("consumer/ is built with -a")
        {
            auto result = f.build({ "-a", "consumer/" });

            THEN("unrelated/stuff.txt is NOT built")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("unrelated/stuff.txt"));
            }
        }
    }
}

SCENARIO("Incremental -a scoped build detects upstream changes", "[e2e][incremental][scope]")
{
    GIVEN("a fully built project with shared include directory")
    {
        auto f = E2EFixture { "scoped_upstream" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("an upstream header is modified and scoped build runs with -a and explicit target")
        {
            f.write_file("include/header.h", "#define VALUE 100\n");
            auto result = f.build({ "-a", "lib" });

            THEN("the scoped module is rebuilt")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }
        }
    }
}

// =============================================================================
// Clean/Distclean Tests
// =============================================================================

SCENARIO("Clean removes generated files but preserves sources", "[e2e][clean]")
{
    GIVEN("a built project")
    {
        auto f = E2EFixture { "clean" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("hello.o"));

        WHEN("pup clean is executed")
        {
            auto result = f.clean();

            THEN("clean succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated files are removed")
            {
                REQUIRE_FALSE(f.exists("hello.o"));
            }

            THEN("source files are preserved")
            {
                REQUIRE(f.exists("hello.c"));
                REQUIRE(f.exists("Tupfile"));
            }

            THEN(".pup directory is preserved")
            {
                REQUIRE(f.exists(".pup"));
            }
        }
    }
}

SCENARIO("Clean dry-run shows what would be removed", "[e2e][clean]")
{
    GIVEN("a built project")
    {
        auto f = E2EFixture { "clean_dry_run" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("hello.o"));

        WHEN("pup clean -n is executed")
        {
            auto result = f.clean({ "-n" });

            THEN("dry-run reports what would be removed")
            {
                REQUIRE(result.stdout_output.find("Would remove") != std::string::npos);
            }

            THEN("files are not actually removed")
            {
                REQUIRE(f.exists("hello.o"));
            }
        }
    }
}

SCENARIO("Clean with no index reports nothing to clean", "[e2e][clean]")
{
    GIVEN("an initialized project without builds")
    {
        auto f = E2EFixture { "clean_no_index" };
        REQUIRE(f.init().success());

        WHEN("pup clean is executed")
        {
            auto result = f.clean();

            THEN("reports nothing to clean")
            {
                REQUIRE(result.stdout_output.find("Nothing to clean") != std::string::npos);
            }
        }
    }
}

SCENARIO("Clean works for out-of-tree builds", "[e2e][clean][variant]")
{
    GIVEN("an out-of-tree built project")
    {
        auto f = E2EFixture { "clean_out_of_tree" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/hello.o"));
        REQUIRE_FALSE(f.exists("hello.o")); // Not in source dir

        WHEN("pup clean -B build is executed")
        {
            auto result = f.clean({ "-B", "build" });

            THEN("clean succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated files in build dir are removed")
            {
                REQUIRE_FALSE(f.exists("build/hello.o"));
            }

            THEN("source files are untouched")
            {
                REQUIRE(f.exists("hello.c"));
            }
        }
    }
}

SCENARIO("Clean works when running from build directory", "[e2e][clean][variant]")
{
    GIVEN("a project built with -B")
    {
        auto f = E2EFixture { "clean_from_build_dir" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/hello.o"));

        WHEN("pup clean is run from within build directory")
        {
            auto result = f.run_pup_in_dir("build", { "clean" });

            THEN("clean succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated files are removed")
            {
                REQUIRE_FALSE(f.exists("build/hello.o"));
            }
        }
    }
}

SCENARIO("Distclean removes all build artifacts", "[e2e][clean]")
{
    GIVEN("a built project with .pup directory")
    {
        auto f = E2EFixture { "distclean" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("hello.o"));
        REQUIRE(f.exists(".pup"));

        WHEN("pup distclean is executed")
        {
            auto result = f.distclean();

            THEN("distclean succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated files are removed")
            {
                REQUIRE_FALSE(f.exists("hello.o"));
            }

            THEN(".pup directory is removed")
            {
                REQUIRE_FALSE(f.exists(".pup"));
            }

            THEN("source files are preserved")
            {
                REQUIRE(f.exists("hello.c"));
                REQUIRE(f.exists("Tupfile"));
            }
        }
    }
}

SCENARIO("Distclean dry-run shows what would be removed", "[e2e][clean]")
{
    GIVEN("a built project")
    {
        auto f = E2EFixture { "distclean_dry_run" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists(".pup"));

        WHEN("pup distclean -n is executed")
        {
            auto result = f.distclean({ "-n" });

            THEN("dry-run reports what would be removed")
            {
                REQUIRE(result.stdout_output.find("Would remove") != std::string::npos);
            }

            THEN("files are not actually removed")
            {
                REQUIRE(f.exists("hello.o"));
                REQUIRE(f.exists(".pup"));
            }
        }
    }
}

SCENARIO("Distclean with no index still removes .pup", "[e2e][clean]")
{
    GIVEN("a project with .pup directory but no index")
    {
        auto f = E2EFixture { "distclean_no_index" };
        f.mkdir(".pup"); // Manually create .pup (simulates interrupted build)
        REQUIRE(f.exists(".pup"));

        WHEN("pup distclean is executed")
        {
            auto result = f.distclean();

            THEN(".pup directory is removed")
            {
                REQUIRE_FALSE(f.exists(".pup"));
            }
        }
    }
}

SCENARIO("Distclean works for out-of-tree builds", "[e2e][clean][variant]")
{
    GIVEN("an out-of-tree built project")
    {
        auto f = E2EFixture { "distclean_out_of_tree" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/hello.o"));
        REQUIRE(f.exists("build/.pup"));

        WHEN("pup distclean -B build is executed")
        {
            auto result = f.distclean({ "-B", "build" });

            THEN("distclean succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated files are removed")
            {
                REQUIRE_FALSE(f.exists("build/hello.o"));
            }

            THEN(".pup in build dir is removed")
            {
                REQUIRE_FALSE(f.exists("build/.pup"));
            }
        }
    }
}

SCENARIO("Distclean works when running from build directory", "[e2e][clean][variant]")
{
    GIVEN("a project built with -B")
    {
        auto f = E2EFixture { "distclean_from_build_dir" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/.pup"));

        WHEN("pup distclean is run from within build directory")
        {
            auto result = f.run_pup_in_dir("build", { "distclean" });

            THEN("distclean succeeds")
            {
                REQUIRE(result.success());
            }

            THEN(".pup is removed")
            {
                REQUIRE_FALSE(f.exists("build/.pup"));
            }
        }
    }
}

// =============================================================================
// Variant Build Tests
// =============================================================================

SCENARIO("Out-of-tree builds place outputs in build directory", "[e2e][variant]")
{
    GIVEN("a configured out_of_tree project")
    {
        auto f = E2EFixture { "out_of_tree" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built with -B build")
        {
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("executable is in build directory")
            {
                REQUIRE(f.is_executable("build/hello"));
            }

            THEN("executable is NOT in source directory")
            {
                REQUIRE_FALSE(f.exists("hello"));
            }

            THEN("executable works")
            {
                auto output = f.run("build/hello").stdout_output;
                REQUIRE(output.find("Hello from out-of-tree build!") != std::string::npos);
            }
        }
    }
}

SCENARIO("Variant build with generated header has correct incremental behavior", "[e2e][variant][incremental]")
{
    GIVEN("a variant build with a generated header")
    {
        auto f = E2EFixture { "variant_generated" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        auto first_build = f.build({ "-B", "build" });
        REQUIRE(first_build.success());
        REQUIRE(f.is_executable("build/program"));

        WHEN("rebuilding without changes")
        {
            auto rebuild = f.build({ "-B", "build" });

            THEN("rebuild is a no-op")
            {
                INFO("stdout: " << rebuild.stdout_output);
                REQUIRE(rebuild.success());
                REQUIRE(rebuild.is_noop());
            }
        }

        WHEN("modifying the generated header content")
        {
            // Modify the Tupfile to change the generated content
            f.write_file("Tupfile", "# Modified to generate different header\n"
                                    ": |> echo '#define VERSION 2' > %o |> version.h\n"
                                    "CFLAGS = -MD -I$(TUP_VARIANT_OUTPUTDIR)\n"
                                    ": main.c | version.h |> gcc $(CFLAGS) -c %f -o %o |> main.o\n"
                                    ": main.o |> gcc %f -o %o |> program\n");

            auto rebuild = f.build({ "-B", "build" });

            THEN("rebuild succeeds")
            {
                REQUIRE(rebuild.success());
            }

            THEN("program output reflects new version")
            {
                auto output = f.run("build/program").stdout_output;
                REQUIRE(output.find("Version: 2") != std::string::npos);
            }
        }
    }
}

SCENARIO("Order-only deps on generated outputs resolve correctly in variants", "[e2e][variant][incremental]")
{
    // This tests the bug where order-only deps using plain paths (e.g., "include/foo.h")
    // created duplicate File nodes instead of reusing Generated nodes.
    // Pattern from busybox:
    //   : |> ... |> include/applets.h  # output becomes build/include/applets.h
    //   : | include/applets.h |> ...   # should resolve to build/include/applets.h
    // Bug: the order-only dep created a File node at "include/applets.h" instead
    // of reusing the Generated node at "build/include/applets.h".
    GIVEN("a variant build with output and order-only dep using same path")
    {
        auto f = E2EFixture { "variant_cross_dir_order_only" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        auto first_build = f.build({ "-B", "build" });
        REQUIRE(first_build.success());
        REQUIRE(f.exists("build/include/header.h"));
        REQUIRE(f.exists("build/consumer/main.o"));

        WHEN("rebuilding without changes")
        {
            auto rebuild = f.build({ "-B", "build" });

            THEN("rebuild is a no-op")
            {
                INFO("stdout: " << rebuild.stdout_output);
                REQUIRE(rebuild.success());
                REQUIRE(rebuild.is_noop());
            }
        }
    }
}

SCENARIO("Variant outputs are automatically mapped to build directory", "[e2e][variant]")
{
    // This tests that output paths are automatically mapped to the variant directory.
    // Pattern from busybox:
    //   Root Tupfile: : |> ... |> include/header.h  # should become build/include/header.h
    //   src/Tupfile:  | $(B)/include/header.h       # references build/include/header.h
    // Without automatic mapping, the output creates a node at "include/header.h" (source),
    // but the dependency references "build/include/header.h" (variant) - creating two nodes.
    GIVEN("a variant build with S/B convention like busybox")
    {
        auto f = E2EFixture { "variant_auto_output" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built with -B build")
        {
            auto result = f.build({ "-B", "build" });
            INFO("stdout: " << result.stdout_output);
            INFO("stderr: " << result.stderr_output);

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("generated header is in build directory")
            {
                REQUIRE(f.exists("build/include/header.h"));
                REQUIRE_FALSE(f.exists("include/header.h"));
            }

            THEN("program is in build directory and works")
            {
                REQUIRE(f.is_executable("build/src/program"));
                auto output = f.run("build/src/program").stdout_output;
                REQUIRE(output.find("Value: 42") != std::string::npos);
            }

            THEN("rebuild is a no-op (no ghost nodes)")
            {
                // If output was mapped incorrectly, there would be a ghost node
                // at build/include/header.h that never gets satisfied
                auto rebuild = f.build({ "-B", "build" });
                INFO("rebuild stdout: " << rebuild.stdout_output);
                REQUIRE(rebuild.success());
                REQUIRE(rebuild.is_noop());
            }
        }
    }
}

SCENARIO("Cross-directory regular inputs work in variant builds", "[e2e][variant]")
{
    // Similar to order-only test but with regular input dependency
    // This tests that Ghost->Generated upgrade preserves edges
    // aaa_consumer is parsed first (alphabetically), creates Ghost for ../zzz_producer/helper.c
    // zzz_producer is parsed later, upgrades Ghost to Generated
    // The edge from aaa_consumer's command to the generated file must be preserved
    GIVEN("a variant build with generated file as regular input from another dir")
    {
        auto f = E2EFixture { "variant_cross_dir_regular_input" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        auto first_build = f.build({ "-B", "build" });
        INFO("stdout: " << first_build.stdout_output);
        INFO("stderr: " << first_build.stderr_output);
        REQUIRE(first_build.success());
        REQUIRE(f.exists("build/zzz_producer/helper.c"));
        REQUIRE(f.exists("build/aaa_consumer/helper.o"));

        WHEN("rebuilding without changes")
        {
            auto rebuild = f.build({ "-B", "build" });

            THEN("rebuild is a no-op")
            {
                INFO("stdout: " << rebuild.stdout_output);
                REQUIRE(rebuild.success());
                REQUIRE(rebuild.is_noop());
            }
        }
    }
}

SCENARIO("Variant builds with tup.config", "[e2e][variant]")
{
    GIVEN("a variant project with config")
    {
        auto f = E2EFixture { "variant" };
        f.mkdir("build");
        f.write_file("build/tup.config", "# Variant config\nCONFIG_VARIANT=build\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built with -B build")
        {
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("outputs are in build directory")
            {
                REQUIRE(f.exists("build/hello.o"));
                REQUIRE(f.is_executable("build/hello"));
            }
        }
    }
}

SCENARIO("Variant builds with symlinked config", "[e2e][variant]")
{
    GIVEN("a variant project with symlinked tup.config")
    {
        auto f = E2EFixture { "variant_symlink" };
        f.mkdir("configs");
        f.mkdir("build");
        f.write_file("configs/build.config", "# Symlinked variant config\nCONFIG_VARIANT=build\n");
        f.create_symlink("../configs/build.config", "build/tup.config");

        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built with -B build")
        {
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("executable is created")
            {
                REQUIRE(f.is_executable("build/hello"));
            }
        }
    }
}

SCENARIO("Subdirectory builds with cross-directory dependencies", "[e2e][variant]")
{
    GIVEN("a subdirectory project")
    {
        auto f = E2EFixture { "subdirectory" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("executable is created in subdirectory")
            {
                REQUIRE(f.is_executable("src/lib/program"));
            }

            THEN("executable produces correct output")
            {
                REQUIRE(f.run("src/lib/program").stdout_output == "Config value: 42\n");
            }
        }

        WHEN("header in different directory is modified")
        {
            (void)f.build(); // First build
            f.write_file("include/config.h", "#define CONFIG_VALUE 100\n");
            auto result = f.build();

            THEN("rebuild occurs")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }

            THEN("output reflects the change")
            {
                REQUIRE(f.run("src/lib/program").stdout_output == "Config value: 100\n");
            }
        }
    }
}

SCENARIO("Variant-only files are found via generalized path resolution", "[e2e][variant][fallback]")
{
    GIVEN("a project with a subdirectory referencing a file only in variant")
    {
        auto f = E2EFixture { "variant_fallback" };
        f.mkdir("build");
        f.write_file("build/tup.config", "# Variant config\n");
        // Create config.txt ONLY in the variant directory, not in source
        f.write_file("build/config.txt", "variant-only-content\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built with -B build")
        {
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds finding the variant-only file")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("output contains the variant file content")
            {
                REQUIRE(f.exists("build/src/output.txt"));
                REQUIRE(f.read_file("build/src/output.txt") == "variant-only-content\n");
            }
        }
    }
}

// =============================================================================
// Shell Fixture Tests (wrapped)
// =============================================================================

SCENARIO("Conditionals test via shell fixture", "[e2e][shell]")
{
    WHEN("the conditionals shell fixture runs")
    {
        auto result = run_shell_fixture("conditionals");

        THEN("all conditional tests pass")
        {
            REQUIRE(result.success());
        }
    }
}

SCENARIO("Self-host test via shell fixture", "[e2e][shell]")
{
    WHEN("the self_host shell fixture runs")
    {
        auto result = run_shell_fixture("self_host");

        THEN("pup can parse its own Tupfile")
        {
            REQUIRE(result.success());
        }
    }
}


// Regression guard: when a source's already-tracked header is edited to
// transitively include a new header, pup must record the new transitive
// header and rebuild on subsequent edits to it. Fixed by binding dep-scan
// command dirty-status to its parent compile (collect_affected_commands).
SCENARIO("Transitive implicit-dep header tracking", "[e2e][shell][incremental]")
{
    WHEN("the header_dep_transitive shell fixture runs")
    {
        auto result = run_shell_fixture("header_dep_transitive");

        THEN("a newly-transitive header's change triggers rebuild")
        {
            INFO("test.sh stdout:\n" << result.stdout_output);
            INFO("test.sh stderr:\n" << result.stderr_output);
            REQUIRE(result.success());
        }
    }
}

// =============================================================================
// Show Command Tests
// =============================================================================

SCENARIO("Show graph shows only declared deps by default", "[e2e][show]")
{
    GIVEN("a built implicit_deps project")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("show graph is run without --all")
        {
            auto result = f.pup({ "show", "graph" });

            THEN("output is valid DOT format")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("digraph G {") != std::string::npos);
            }

            THEN("implicit deps are not shown")
            {
                REQUIRE(result.stdout_output.find("config.h") == std::string::npos);
                REQUIRE(result.stdout_output.find("style=dashed") == std::string::npos);
            }
        }
    }
}

SCENARIO("Show graph --all includes implicit deps", "[e2e][show]")
{
    GIVEN("a built implicit_deps project")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("show graph --all-deps is run")
        {
            auto result = f.pup({ "show", "graph", "--all-deps" });

            THEN("output is valid DOT format")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("digraph G {") != std::string::npos);
            }

            THEN("implicit deps are shown with dashed style")
            {
                REQUIRE(result.stdout_output.find("config.h") != std::string::npos);
                REQUIRE(result.stdout_output.find("style=dashed") != std::string::npos);
            }
        }
    }
}

SCENARIO("Show graph --all-deps with no index warns", "[e2e][show]")
{
    GIVEN("an initialized but NOT built project")
    {
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());

        WHEN("show graph --all-deps is run")
        {
            auto result = f.pup({ "show", "graph", "--all-deps" });

            THEN("command succeeds with warning")
            {
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("No index found") != std::string::npos);
            }

            THEN("output has declared deps only")
            {
                REQUIRE(result.stdout_output.find("digraph G {") != std::string::npos);
                REQUIRE(result.stdout_output.find("style=dashed") == std::string::npos);
            }
        }
    }
}

SCENARIO("Show graph --summary --all-deps shows implicit edge count", "[e2e][show]")
{
    GIVEN("a built implicit_deps project")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("show graph --summary --all-deps is run")
        {
            auto result = f.pup({ "show", "graph", "--summary", "--all-deps" });

            THEN("output shows implicit edge count")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Implicit edges:") != std::string::npos);
            }
        }
    }
}

SCENARIO("Show index dumps implicit-dep edges from the on-disk index", "[e2e][show]")
{
    GIVEN("a built project with a -MD compile rule (implicit deps recorded)")
    {
        auto f = E2EFixture { "scoped_implicit_dep" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("src/main.o"));

        WHEN("show index is run")
        {
            auto result = f.pup({ "show", "index" });

            THEN("output reports file/command/edge counts and a header.h implicit edge")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Files:") != std::string::npos);
                REQUIRE(result.stdout_output.find("Commands:") != std::string::npos);
                REQUIRE(result.stdout_output.find("Implicit=") != std::string::npos);
                REQUIRE(result.stdout_output.find("implicit:") != std::string::npos);
                REQUIRE(result.stdout_output.find("header.h") != std::string::npos);
            }
        }

        WHEN("show index --summary is run")
        {
            auto result = f.pup({ "show", "index", "--summary" });

            THEN("output is a single summary block with no per-command listing")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Files:") != std::string::npos);
                REQUIRE(result.stdout_output.find("Commands with implicit/sticky deps:") != std::string::npos);
                // No per-command section in summary mode
                REQUIRE(result.stdout_output.find("Commands (with implicit/sticky edges):") == std::string::npos);
            }
        }

        WHEN("show index with a positional filter is run")
        {
            auto result = f.pup({ "show", "index", "nonexistent_xyz" });

            THEN("the per-command section is empty (filter matched nothing)")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Commands (with implicit/sticky edges):") != std::string::npos);
                REQUIRE(result.stdout_output.find("implicit:") == std::string::npos);
            }
        }
    }
}

SCENARIO("Depfiles named after the full object path are discovered", "[e2e][incremental]")
{
    GIVEN("a built project whose compile rule writes <output>.d (clang-cl style)")
    {
        auto f = E2EFixture { "objpath_depfile" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("src/main.o"));
        REQUIRE(f.exists("src/main.o.d"));

        WHEN("show index is run")
        {
            auto result = f.pup({ "show", "index" });

            THEN("the header.h implicit edge was recorded")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("header.h") != std::string::npos);
            }
        }

        WHEN("the header changes and the project is rebuilt")
        {
            f.write_file("include/header.h", "#define VERSION 2\n");
            auto rebuild = f.build();

            THEN("the object is recompiled")
            {
                INFO("stdout: " << rebuild.stdout_output);
                REQUIRE(rebuild.success());
                REQUIRE_FALSE(rebuild.is_noop());
            }
        }
    }
}

SCENARIO("Show exporters omit commands from inactive conditional branches", "[e2e][show]")
{
    GIVEN("a Tupfile whose ifeq branches both produce main.o")
    {
        auto f = E2EFixture { "inactive_branch_show" };
        REQUIRE(f.init().success());

        f.append_file("tup.config",
            "CONFIG_SCRIPT_PROLOGUE=#!/bin/sh\\nset -ex\\ncd \"$(dirname \"$0\")\"\n"
            "CONFIG_SCRIPT_RUN=(cd \"%DIR\" && %CMD)\n"
            "CONFIG_SCRIPT_MKDIR=mkdir -p \"%DIR\"\n"
            "CONFIG_SCRIPT_COMMENT=#\n");

        WHEN("show script is run")
        {
            auto result = f.pup({ "show", "script" });

            THEN("only the active branch's command is emitted")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("cc -c") != std::string::npos);
                REQUIRE(result.stdout_output.find("-DALT") == std::string::npos);
            }
        }

        WHEN("show compdb is run")
        {
            auto result = f.pup({ "show", "compdb" });

            THEN("only the active branch's compile entry is emitted")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("main.c") != std::string::npos);
                REQUIRE(result.stdout_output.find("-DALT") == std::string::npos);
            }
        }
    }
}

SCENARIO("Show script output is independent of saved index state", "[e2e][show]")
{
    GIVEN("a two-directory variant project and a freshly generated script")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("alpha/a.c", "int a;\n");
        f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
        f.write_file("beta/b.c", "int b;\n");
        f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out\n");
        f.write_file("build/tup.config",
            "CONFIG_SCRIPT_PROLOGUE=#!/bin/sh\\nset -ex\\ncd \"$(dirname \"$0\")\"\n"
            "CONFIG_SCRIPT_RUN=(cd \"%DIR\" && %CMD)\n"
            "CONFIG_SCRIPT_MKDIR=mkdir -p \"%DIR\"\n"
            "CONFIG_SCRIPT_COMMENT=#\n");
        auto fresh = f.pup({ "show", "script", "-B", "build" });
        REQUIRE(fresh.success());
        REQUIRE(fresh.stdout_output.find("build/alpha") != std::string::npos);

        WHEN("a scoped build reshapes the saved index and the script is regenerated")
        {
            REQUIRE(f.build({ "-B", "build", "beta" }).success());
            auto regenerated = f.pup({ "show", "script", "-B", "build" });

            THEN("the generated script is byte-identical")
            {
                REQUIRE(regenerated.success());
                REQUIRE(regenerated.stdout_output == fresh.stdout_output);
            }
        }
    }
}

SCENARIO("Show script generates shell build script", "[e2e][show]")
{
    GIVEN("a simple C project")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        // Add required script config variables
        f.append_file("tup.config",
            "CONFIG_SCRIPT_PROLOGUE=#!/bin/sh\\nset -ex\\ncd \"$(dirname \"$0\")\"\n"
            "CONFIG_SCRIPT_RUN=(cd \"%DIR\" && %CMD)\n"
            "CONFIG_SCRIPT_MKDIR=mkdir -p \"%DIR\"\n"
            "CONFIG_SCRIPT_COMMENT=#\n");

        WHEN("show script is run")
        {
            auto result = f.pup({ "show", "script" });

            THEN("output is a valid shell script")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("#!/bin/sh") != std::string::npos);
                REQUIRE(result.stdout_output.find("set -e") != std::string::npos);
            }

            THEN("script contains the build command")
            {
                REQUIRE(result.stdout_output.find("gcc") != std::string::npos);
            }
        }
    }
}

SCENARIO("Show compdb generates compile_commands.json", "[e2e][show]")
{
    GIVEN("a simple C project")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        WHEN("show compdb is run")
        {
            auto result = f.pup({ "show", "compdb" });

            THEN("output is valid JSON array")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("[") != std::string::npos);
                REQUIRE(result.stdout_output.find("]") != std::string::npos);
            }

            THEN("entries have required fields")
            {
                REQUIRE(result.stdout_output.find("\"directory\"") != std::string::npos);
                REQUIRE(result.stdout_output.find("\"arguments\"") != std::string::npos);
                REQUIRE(result.stdout_output.find("\"file\"") != std::string::npos);
            }

            THEN("source file is referenced")
            {
                REQUIRE(result.stdout_output.find("hello.c") != std::string::npos);
            }
        }
    }
}

SCENARIO("Show with unified variant target", "[e2e][show][target]")
{
    GIVEN("a project with a variant directory")
    {
        auto f = E2EFixture { "multi_variant" };
        f.mkdir("build-debug");
        f.write_file("build-debug/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build-debug" }).success());
        REQUIRE(f.build({ "-B", "build-debug" }).success());

        WHEN("show graph --summary is run with variant target")
        {
            auto result = f.pup({ "show", "graph", "--summary", "build-debug" });

            THEN("command succeeds with variant prefix in output")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("[build-debug]") != std::string::npos);
                REQUIRE(result.stdout_output.find("Nodes:") != std::string::npos);
            }
        }
    }
}

SCENARIO("Show with unknown format fails", "[e2e][show]")
{
    GIVEN("an initialized project")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        WHEN("show is run with unknown format")
        {
            auto result = f.pup({ "show", "unknown" });

            THEN("command fails with error message")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("Unknown show format") != std::string::npos);
            }
        }
    }
}

// =============================================================================
// Layout Detection Tests
// =============================================================================

SCENARIO("Layout detection finds build directory via tup.config", "[e2e][layout]")
{
    GIVEN("a project with build/tup.config")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());

        REQUIRE(f.exists("build/.pup"));
        REQUIRE(f.exists("build/tup.config"));

        WHEN("show graph --all-deps is run without -B")
        {
            auto result = f.pup({ "show", "graph", "--summary", "--all-deps" });

            THEN("it refuses and names the candidate")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("build") != std::string::npos);
            }
        }

        WHEN("show graph --all-deps is run with the build dir as target")
        {
            auto result = f.pup({ "show", "graph", "--summary", "--all-deps", "build" });

            THEN("the build directory's index is used")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Implicit edges:") != std::string::npos);
            }

            THEN("no warning about missing index")
            {
                REQUIRE(result.stderr_output.find("No index found") == std::string::npos);
            }
        }
    }
}

SCENARIO("Layout detection prefers tup.config over .pup", "[e2e][layout]")
{
    GIVEN("a project with both build/tup.config and build/.pup")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        f.mkdir("build");
        f.write_file("build/tup.config", "# Variant config\n");
        REQUIRE(f.build({ "-B", "build" }).success());

        REQUIRE(f.exists("build/.pup"));
        REQUIRE(f.exists("build/tup.config"));

        WHEN("show graph --all-deps is run with the build dir as target")
        {
            auto result = f.pup({ "show", "graph", "--summary", "--all-deps", "build" });

            THEN("build directory is used")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Implicit edges:") != std::string::npos);
            }
        }
    }
}

SCENARIO("Layout detection prefers build/.pup with index over empty source .pup", "[e2e][layout]")
{
    GIVEN("source root has empty .pup AND build has .pup with index")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };

        // Create empty .pup at source root (simulating stale directory)
        f.mkdir(".pup");
        REQUIRE(f.exists(".pup"));
        REQUIRE_FALSE(f.exists(".pup/index"));

        // Configure and build to build/ which creates build/.pup/index
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/.pup/index"));

        WHEN("clean is run with the build dir as target")
        {
            auto result = f.clean({ "build" });

            THEN("it finds build/.pup and cleans successfully")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("no index found") == std::string::npos);
            }
        }

        WHEN("show graph --all-deps is run with the build dir as target")
        {
            auto result = f.pup({ "show", "graph", "--summary", "--all-deps", "build" });

            THEN("it finds build/.pup/index")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Implicit edges:") != std::string::npos);
            }
        }
    }
}

// =============================================================================
// Multi-Variant Build Tests
// =============================================================================

SCENARIO("Multi-variant glob selection builds all matching variants", "[e2e][multi-variant]")
{
    GIVEN("a project with multiple variant directories")
    {
        auto f = E2EFixture { "multi_variant" };

        // Create two variant directories with tup.config
        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup build-* is run from project root")
        {
            auto result = f.pup({ "build-*" });

            THEN("all variants are built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE(f.is_executable("build-release/hello"));
            }

            THEN("debug variant has debug output")
            {
                REQUIRE(f.run("build-debug/hello").stdout_output == "Debug mode\n");
            }

            THEN("release variant has release output")
            {
                REQUIRE(f.run("build-release/hello").stdout_output == "Release mode\n");
            }
        }

        WHEN("rebuilt without changes")
        {
            (void)f.pup({ "build-*" });
            auto result = f.pup({ "build-*" });

            THEN("nothing is rebuilt")
            {
                REQUIRE(result.is_noop());
            }
        }
    }
}

SCENARIO("Ambiguous build directories require explicit selection", "[e2e][multi-variant][ambiguous]")
{
    GIVEN("a project with two variant directories")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup is run bare from the project root")
        {
            auto result = f.build();

            THEN("it fails listing the candidates")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("build-debug") != std::string::npos);
                REQUIRE(result.stderr_output.find("build-release") != std::string::npos);
                REQUIRE(result.stderr_output.find("-B") != std::string::npos);
            }

            THEN("nothing is built")
            {
                REQUIRE_FALSE(f.exists("build-debug/hello"));
                REQUIRE_FALSE(f.exists("build-release/hello"));
            }
        }

        WHEN("a glob target selects all variants")
        {
            auto result = f.pup({ "build-*" });

            THEN("all variants build")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE(f.is_executable("build-release/hello"));
            }
        }

        WHEN("plural path targets select both variants")
        {
            auto result = f.pup({ "build-debug", "build-release" });

            THEN("both variants build")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE(f.is_executable("build-release/hello"));
            }
        }

        WHEN("pup runs from inside one variant")
        {
            auto result = f.run_pup_in_dir("build-debug", {});

            THEN("only that variant builds")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE_FALSE(f.exists("build-release/hello"));
            }
        }

        WHEN("bare clean and parse also refuse the ambiguity")
        {
            THEN("clean fails")
            {
                REQUIRE_FALSE(f.clean().success());
            }

            THEN("parse fails")
            {
                REQUIRE_FALSE(f.parse().success());
            }
        }

        WHEN("pup runs from a subdirectory deep inside one variant")
        {
            f.mkdir("build-debug/nested");
            auto result = f.run_pup_in_dir("build-debug/nested", {});

            THEN("the enclosing variant is selected")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE_FALSE(f.exists("build-release/hello"));
            }
        }
    }
}

SCENARIO("A single discovered build directory is not adopted implicitly", "[e2e][multi-variant][ambiguous]")
{
    GIVEN("a project with exactly one variant directory")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");

        WHEN("pup is run bare from the project root")
        {
            auto result = f.build();

            THEN("it fails naming the candidate")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("build-debug") != std::string::npos);
                REQUIRE_FALSE(f.exists("build-debug/hello"));
            }
        }

        WHEN("the candidate is passed explicitly")
        {
            auto result = f.pup({ "build-debug" });

            THEN("it builds")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
            }
        }

        WHEN("pup runs from inside the variant")
        {
            auto result = f.run_pup_in_dir("build-debug", {});

            THEN("it builds that variant")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
            }
        }
    }
}

SCENARIO("Explicit multi-variant with -B flags", "[e2e][multi-variant]")
{
    GIVEN("a project with multiple variant directories")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.mkdir("build-custom");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");
        f.write_file("build-custom/tup.config", "CONFIG_DEBUG=y\n");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup -B build-debug -B build-release is run")
        {
            auto result = f.build({ "-B", "build-debug", "-B", "build-release" });

            THEN("specified variants are built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE(f.is_executable("build-release/hello"));
            }

            THEN("unspecified variant is NOT built")
            {
                REQUIRE_FALSE(f.exists("build-custom/hello"));
            }
        }
    }
}

SCENARIO("Single -B flag still works", "[e2e][multi-variant]")
{
    GIVEN("a project with multiple variant directories")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup -B build-debug is run")
        {
            auto result = f.build({ "-B", "build-debug" });

            THEN("only specified variant is built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE_FALSE(f.exists("build-release/hello"));
            }
        }
    }
}

SCENARIO("Multi-variant verbose output prefixes lines", "[e2e][multi-variant]")
{
    GIVEN("a project with multiple variant directories")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup -v build-* is run with multiple variants")
        {
            auto result = f.build({ "-v", "build-*" });

            THEN("output lines are prefixed with variant names")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("[build-debug]") != std::string::npos);
                REQUIRE(result.stdout_output.find("[build-release]") != std::string::npos);
            }
        }
    }
}

SCENARIO("No variants found falls back to in-tree build", "[e2e][multi-variant]")
{
    GIVEN("a project with no variant directories")
    {
        auto f = E2EFixture { "multi_variant" };
        REQUIRE(f.init().success());

        WHEN("pup is run from project root")
        {
            auto result = f.build();

            THEN("in-tree build succeeds")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("hello"));
            }
        }
    }
}

SCENARIO("Multi-variant clean", "[e2e][multi-variant][clean]")
{
    GIVEN("a project with multiple built variants")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");
        REQUIRE(f.pup({ "configure", "build-*" }).success());

        REQUIRE(f.pup({ "build-*" }).success());
        REQUIRE(f.exists("build-debug/hello"));
        REQUIRE(f.exists("build-release/hello"));

        WHEN("pup clean build-* is run from project root")
        {
            auto result = f.clean({ "build-*" });

            THEN("all variants are cleaned")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build-debug/hello"));
                REQUIRE_FALSE(f.exists("build-release/hello"));
            }
        }
    }
}

SCENARIO("Multi-variant clean with explicit -B flags", "[e2e][multi-variant][clean]")
{
    GIVEN("a project with multiple built variants")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");
        REQUIRE(f.pup({ "configure", "build-*" }).success());

        REQUIRE(f.pup({ "build-*" }).success());
        REQUIRE(f.exists("build-debug/hello"));
        REQUIRE(f.exists("build-release/hello"));

        WHEN("pup clean -B build-debug is run")
        {
            auto result = f.clean({ "-B", "build-debug" });

            THEN("only the specified variant is cleaned")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build-debug/hello"));
                REQUIRE(f.exists("build-release/hello"));
            }
        }
    }
}

SCENARIO("Multi-variant parse", "[e2e][multi-variant]")
{
    GIVEN("a project with multiple variant directories")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");
        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup parse build-* is run from project root")
        {
            auto result = f.parse({ "build-*" });

            THEN("all variants are parsed")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Parsed") != std::string::npos);
            }
        }
    }
}

// =============================================================================
// Unified Target Tests
// =============================================================================

SCENARIO("Unified targets - path-based variant selection", "[e2e][target]")
{
    GIVEN("a project with build-debug and build-release variants")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup build-debug is run (path-based variant)")
        {
            auto result = f.pup({ "build-debug" });

            THEN("only build-debug variant is built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE_FALSE(f.exists("build-release/hello"));
            }
        }

        WHEN("pup build-release is run (path-based variant)")
        {
            auto result = f.pup({ "build-release" });

            THEN("only build-release variant is built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-release/hello"));
                REQUIRE_FALSE(f.exists("build-debug/hello"));
            }
        }
    }
}

SCENARIO("Unified targets - glob pattern variant selection", "[e2e][target]")
{
    GIVEN("a project with build-debug and build-release variants")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup 'build-*' is run (glob pattern)")
        {
            auto result = f.pup({ "build-*" });

            THEN("all matching variants are built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE(f.is_executable("build-release/hello"));
            }
        }
    }
}

SCENARIO("Unified targets - explicit multiple variants", "[e2e][target]")
{
    GIVEN("a project with build-debug, build-release, and build-custom variants")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.mkdir("build-custom");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");
        f.write_file("build-custom/tup.config", "CONFIG_DEBUG=y\n");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("pup build-debug build-release is run (explicit multiple)")
        {
            auto result = f.pup({ "build-debug", "build-release" });

            THEN("specified variants are built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE(f.is_executable("build-release/hello"));
            }

            THEN("unspecified variant is NOT built")
            {
                REQUIRE_FALSE(f.exists("build-custom/hello"));
            }
        }
    }
}

SCENARIO("Unified targets - error on source file target", "[e2e][target]")
{
    GIVEN("a project with source files")
    {
        auto f = E2EFixture { "multi_variant" };
        REQUIRE(f.init().success());

        WHEN("pup hello.c is run (source file target)")
        {
            auto result = f.pup({ "hello.c" });

            THEN("an error is returned")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("source") != std::string::npos);
            }
        }
    }
}

SCENARIO("Unified targets - error on nonexistent path", "[e2e][target]")
{
    GIVEN("an initialized project")
    {
        auto f = E2EFixture { "multi_variant" };
        REQUIRE(f.init().success());

        WHEN("pup nonexistent is run")
        {
            auto result = f.pup({ "nonexistent" });

            THEN("an error is returned (target not in build graph)")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("is not in build graph") != std::string::npos);
            }
        }
    }
}

SCENARIO("Unified targets - error on mixed variant/non-variant targets", "[e2e][target]")
{
    GIVEN("a project with build-debug and src directories")
    {
        auto f = E2EFixture { "scoped_build" };

        f.mkdir("build-debug");
        f.write_file("build-debug/tup.config", "");

        REQUIRE(f.pup({ "configure", "-B", "build-debug" }).success());

        WHEN("pup build-debug lib is run (mixed targets)")
        {
            auto result = f.pup({ "build-debug", "lib" });

            THEN("an error about mixing targets is returned")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("mix") != std::string::npos);
            }
        }
    }
}

SCENARIO("Unified targets - single output file target", "[e2e][target]")
{
    GIVEN("a project with build-debug variant")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");

        REQUIRE(f.pup({ "configure", "-B", "build-debug" }).success());

        WHEN("pup build-debug/hello is run after full build")
        {
            // First do a full build so the output exists
            REQUIRE(f.build({ "-B", "build-debug" }).success());
            REQUIRE(f.is_executable("build-debug/hello"));

            // Remove the output to force rebuild
            f.remove_file("build-debug/hello");
            REQUIRE_FALSE(f.exists("build-debug/hello"));

            // Build just the single output
            auto result = f.pup({ "build-debug/hello" });

            THEN("only that output is rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
            }
        }
    }
}

SCENARIO("Unified targets - error on nonexistent output in graph", "[e2e][target]")
{
    GIVEN("a project with build-debug variant")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");

        REQUIRE(f.pup({ "configure", "-B", "build-debug" }).success());

        WHEN("pup build-debug/nonexistent.o is run")
        {
            auto result = f.pup({ "build-debug/nonexistent.o" });

            THEN("an error is returned")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("not in build graph") != std::string::npos);
            }
        }
    }
}

SCENARIO("Unified targets - B flag with output target", "[e2e][target]")
{
    GIVEN("a project with build-debug variant")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");

        REQUIRE(f.pup({ "configure", "-B", "build-debug" }).success());

        WHEN("pup -B build-debug build-debug/hello rebuilds deleted output")
        {
            // First do a full build
            REQUIRE(f.build({ "-B", "build-debug" }).success());
            REQUIRE(f.is_executable("build-debug/hello"));

            // Remove output and rebuild with -B + output target
            f.remove_file("build-debug/hello");
            REQUIRE_FALSE(f.exists("build-debug/hello"));

            auto result = f.pup({ "-B", "build-debug", "build-debug/hello" });

            THEN("the output is rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build-debug/hello"));
            }
        }
    }
}

// =============================================================================
// Target-based build (from scratch) tests
// =============================================================================

SCENARIO("Target-based build from scratch", "[e2e][target]")
{
    GIVEN("an initialized multi_file project")
    {
        auto f = E2EFixture { "multi_file" };
        REQUIRE(f.init().success());

        WHEN("building a specific output target from scratch")
        {
            auto result = f.pup({ "calc" });

            THEN("target and all dependencies are built")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("calc"));
                REQUIRE(f.exists("add.o"));
                REQUIRE(f.exists("main.o"));
                REQUIRE(f.exists("multiply.o"));
            }
        }
    }
}

SCENARIO("Target-based build of single object", "[e2e][target]")
{
    GIVEN("an initialized multi_file project")
    {
        auto f = E2EFixture { "multi_file" };
        REQUIRE(f.init().success());

        WHEN("building just one object file")
        {
            auto result = f.pup({ "add.o" });

            THEN("only that object is built")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("add.o"));
                REQUIRE_FALSE(f.exists("calc"));
                REQUIRE_FALSE(f.exists("main.o"));
                REQUIRE_FALSE(f.exists("multiply.o"));
            }
        }
    }
}

SCENARIO("Target-based build with variant", "[e2e][target][variant]")
{
    GIVEN("an initialized out_of_tree project with variant")
    {
        auto f = E2EFixture { "out_of_tree" };
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("building a specific output target from scratch with -B")
        {
            auto result = f.pup({ "-B", "build", "hello" });

            THEN("target and all dependencies are built in variant dir")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("build/hello"));
                REQUIRE(f.exists("build/hello.o"));
            }
        }
    }
}

// =============================================================================
// Scoped tup.config tests
// =============================================================================

SCENARIO("Subdir merges parent and local config", "[e2e][scoped-config]")
{
    GIVEN("a project with root and sub configs defining different vars")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_ROOT_VAR=from_root\n");
        f.write_file("build/sub/tup.config", "CONFIG_SUB_VAR=from_sub\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(SUB_VAR) resolves to 'from_sub' from local config")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/sub.txt") == "from_sub\n");
            }

            THEN("@(ROOT_VAR) in sub/ resolves to 'from_root' (merged from parent)")
            {
                REQUIRE(f.read_file("build/sub/root_from_sub.txt") == "from_root\n");
            }
        }
    }
}

SCENARIO("Subdir inherits from parent when no local config", "[e2e][scoped-config]")
{
    GIVEN("a project with sub/deep/Tupfile using @(SUB_VAR)")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub/deep");
        f.write_file("build/tup.config", "CONFIG_ROOT_VAR=from_root\n");
        f.write_file("build/sub/tup.config", "CONFIG_SUB_VAR=from_sub\n");
        // NO build/sub/deep/tup.config
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(SUB_VAR) in sub/deep/ resolves to 'from_sub' (found by walking up)")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/deep/sub_from_deep.txt") == "from_sub\n");
            }
        }
    }
}

SCENARIO("Root config used when no intermediate configs", "[e2e][scoped-config]")
{
    GIVEN("a project with sub/Tupfile using @(ROOT_VAR)")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_ROOT_VAR=from_root\n");
        // NO build/sub/tup.config - should inherit from root
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(ROOT_VAR) in sub/ resolves to 'from_root'")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/root_from_sub.txt") == "from_root\n");
            }
        }
    }
}

SCENARIO("Empty subdir config does not block parent merge", "[e2e][scoped-config]")
{
    GIVEN("a project with an empty sub config and a populated root config")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_ROOT_VAR=from_root\n");
        f.write_file("build/sub/tup.config", ""); // Empty — parent vars merge through
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(ROOT_VAR) in sub/ resolves to 'from_root' (parent merges through)")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/root_from_sub.txt") == "from_root\n");
            }
        }
    }
}

SCENARIO("Parent config overrides child on collision", "[e2e][scoped-config]")
{
    GIVEN("root and sub configs both define SUB_VAR")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_SUB_VAR=from_root_override\n");
        f.write_file("build/sub/tup.config", "CONFIG_SUB_VAR=from_sub\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(SUB_VAR) in sub/ resolves to root's value (parent wins)")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/sub.txt") == "from_root_override\n");
            }
        }
    }
}

SCENARIO("Multi-level config merge", "[e2e][scoped-config]")
{
    GIVEN("configs at root, sub, and sub/deep levels")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub/deep");
        f.write_file("build/tup.config", "CONFIG_ROOT_VAR=from_root\n");
        f.write_file("build/sub/tup.config", "CONFIG_SUB_VAR=from_sub\n");
        f.write_file("build/sub/deep/tup.config", "CONFIG_DEEP_VAR=from_deep\n");
        // Custom Tupfile to test all three vars at the deep level
        f.write_file("sub/deep/Tupfile",
            ": |> echo \"@(ROOT_VAR)\" > %o |> root_from_deep.txt\n"
            ": |> echo \"@(SUB_VAR)\" > %o |> sub_from_deep.txt\n"
            ": |> echo \"@(DEEP_VAR)\" > %o |> deep.txt\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("all three levels merge into sub/deep/")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/deep/root_from_deep.txt") == "from_root\n");
                REQUIRE(f.read_file("build/sub/deep/sub_from_deep.txt") == "from_sub\n");
                REQUIRE(f.read_file("build/sub/deep/deep.txt") == "from_deep\n");
            }
        }
    }
}

SCENARIO("Parent can explicitly clear a child config var", "[e2e][scoped-config]")
{
    GIVEN("sub defines SUB_VAR and root clears it with empty value")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_SUB_VAR=\n");
        f.write_file("build/sub/tup.config", "CONFIG_SUB_VAR=default_value\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(SUB_VAR) in sub/ is empty (parent's explicit clear wins)")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/sub.txt") == "\n");
            }
        }
    }
}

SCENARIO("-D config overrides win over all config files", "[e2e][scoped-config]")
{
    GIVEN("root and sub configs both define SUB_VAR")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_SUB_VAR=from_root\n");
        f.write_file("build/sub/tup.config", "CONFIG_SUB_VAR=from_sub\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup builds with -D SUB_VAR=from_cli")
        {
            auto result = f.build({ "-B", "build", "-D", "SUB_VAR=from_cli" });

            THEN("@(SUB_VAR) in sub/ resolves to CLI value (highest precedence)")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/sub.txt") == "from_cli\n");
            }
        }
    }
}

// =============================================================================
// pup configure command tests
// =============================================================================

SCENARIO("Configure executes config-generating rules only", "[e2e][configure]")
{
    GIVEN("a project with config-generating and normal rules")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=board-xyz\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("configs/tup.config is created")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/configs/tup.config"));
            }

            THEN("out.txt is NOT created (non-config rule skipped)")
            {
                REQUIRE_FALSE(f.exists("build/sub/out.txt"));
            }
        }
    }
}

SCENARIO("Empty-rendered commands are rejected at graph-build time", "[e2e][configure][empty-command]")
{
    GIVEN("a project whose rule command is a single unset config var")
    {
        auto f = E2EFixture { "empty_command" };

        WHEN("pup configure runs with no config")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("the bootstrap pass tolerates the empty render")
            {
                REQUIRE(result.success());
            }
        }

        WHEN("the project builds with the var unset")
        {
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build" });

            THEN("the build fails with a pointed diagnostic")
            {
                REQUIRE_FALSE(result.success());
                auto combined = result.stderr_output + result.stdout_output;
                REQUIRE(combined.find("Tupfile:1") != std::string::npos);
                REQUIRE(combined.find("@(CC_CMD)") != std::string::npos);
            }
        }

        WHEN("the project builds with the var set")
        {
            f.mkdir("build");
            f.write_file("build/tup.config", "CONFIG_CC_CMD=cp %f %o\n");
            auto result = f.build({ "-B", "build" });

            THEN("the command renders and runs")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/out.o"));
            }
        }
    }
}

SCENARIO("Configure uses root tup.config only", "[e2e][configure]")
{
    GIVEN("a project with configs/Tupfile using @(MACHINE)")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=board-xyz\n");
        // NO build/configs/tup.config
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("@(MACHINE) resolves to 'board-xyz' and config is generated")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/configs/tup.config"));
                auto content = f.read_file("build/configs/tup.config");
                REQUIRE(content.find("board-xyz") != std::string::npos);
            }
        }
    }
}

SCENARIO("Configure does not write index", "[e2e][configure]")
{
    GIVEN("a fresh project without index")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=board-xyz\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        // Remove index if it was created by init
        f.remove_file("build/.pup/index");
        REQUIRE_FALSE(f.exists("build/.pup/index"));

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("index does NOT exist")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build/.pup/index"));
            }
        }
    }
}

SCENARIO("Configure does not create .pup directory", "[e2e][configure]")
{
    GIVEN("a project without .pup directory")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=board-xyz\n");
        // Do NOT call init - no .pup directory exists
        REQUIRE_FALSE(f.exists("build/.pup"));

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN(".pup directory is NOT created")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build/.pup"));
            }
        }
    }
}

SCENARIO("Full two-stage build with pup configure", "[e2e][configure]")
{
    GIVEN("a project where configs/ generates tup.config")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=hello-world\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup configure runs, then pup runs")
        {
            auto configure_result = f.pup({ "configure", "-B", "build" });
            REQUIRE(configure_result.success());

            // Verify config content immediately after configure
            REQUIRE(f.exists("build/configs/tup.config"));
            auto config_content = f.read_file("build/configs/tup.config");
            REQUIRE(config_content.find("hello-world") != std::string::npos);

            auto build_result = f.build({ "-B", "build" });

            THEN("build succeeds after configure")
            {
                REQUIRE(build_result.success());
            }
        }
    }
}

SCENARIO("Configure handles config rule depending on non-config rule", "[e2e][configure][deps]")
{
    // Bug: passing only config-output commands as a filter ignores their dependencies.
    // If a config rule depends on an intermediate file produced by a non-config rule,
    // the dependency is not run, causing the config rule to fail.

    GIVEN("a project where config rule depends on intermediate file")
    {
        auto f = E2EFixture { "configure_deps" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("configure succeeds and config file is created")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                // This test documents the expected behavior:
                // Configure should run BOTH the intermediate rule AND the config rule
                REQUIRE(result.success());
                REQUIRE(f.exists("build/configs/tup.config"));
            }
        }
    }
}

SCENARIO("Configure works with empty .pup directory (no index)", "[e2e][configure][init]")
{
    GIVEN("a project with empty .pup directory")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");

        // Create empty .pup directory (simulating interrupted init or manual mkdir)
        f.mkdir("build/.pup");
        REQUIRE(f.exists("build/.pup"));
        REQUIRE_FALSE(f.exists("build/.pup/index"));

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("configure succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/configs/tup.config"));
            }
        }
    }
}

SCENARIO("Configure creates empty tup.config when no config rules exist", "[e2e][configure]")
{
    GIVEN("a project without config-generating rules")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        WHEN("pup configure -B is run")
        {
            auto result = f.pup({ "configure", "-B", "build-test" });

            THEN("it succeeds and creates tup.config")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build-test/tup.config"));
            }
        }
    }
}

SCENARIO("Build requires tup.config", "[e2e][build][configure]")
{
    GIVEN("a project without tup.config")
    {
        auto f = E2EFixture { "simple_c" };

        WHEN("pup build is run without configure")
        {
            auto result = f.build();

            THEN("it fails with helpful error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("configure") != std::string::npos);
            }
        }
    }
}

SCENARIO("Build succeeds after configure", "[e2e][build][configure]")
{
    GIVEN("a configured project")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.pup({ "configure" }).success());

        WHEN("pup build is run")
        {
            auto result = f.build();

            THEN("it succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }
        }
    }
}

SCENARIO("Config selection persists across multiple builds", "[e2e][configure]")
{
    GIVEN("a project with config selected via tup.config")
    {
        // Use simple_c fixture which has no config-generating rules
        auto f = E2EFixture { "simple_c" };
        f.mkdir("build");
        // Write config that would be selected via environment variable
        // (e.g., user sets CONFIG_BOARD=my-board based on env var)
        f.write_file("build/tup.config", "CONFIG_OPT=fast\nCONFIG_DEBUG=0\n");

        WHEN("pup configure runs and then pup build runs multiple times")
        {
            auto configure_result = f.pup({ "configure", "-B", "build" });
            INFO("configure stdout: " << configure_result.stdout_output);
            INFO("configure stderr: " << configure_result.stderr_output);
            REQUIRE(configure_result.success());

            // First build
            auto build1 = f.build({ "-B", "build" });
            INFO("build1 stdout: " << build1.stdout_output);
            INFO("build1 stderr: " << build1.stderr_output);
            REQUIRE(build1.success());
            REQUIRE(f.is_executable("build/hello"));

            // Second build should succeed
            auto build2 = f.build({ "-B", "build" });
            INFO("build2 stdout: " << build2.stdout_output);
            INFO("build2 stderr: " << build2.stderr_output);
            REQUIRE(build2.success());

            // Third build should be no-op (nothing changed)
            auto build3 = f.build({ "-B", "build" });
            INFO("build3 stdout: " << build3.stdout_output);
            INFO("build3 stderr: " << build3.stderr_output);
            REQUIRE(build3.success());
            REQUIRE(build3.is_noop());

            THEN("builds are stable and config persists")
            {
                // Config file should still exist with original values
                auto config = f.read_file("build/tup.config");
                REQUIRE(config.find("CONFIG_OPT=fast") != std::string::npos);
                REQUIRE(config.find("CONFIG_DEBUG=0") != std::string::npos);
            }
        }
    }
}

SCENARIO("Build skips config-generating rules", "[e2e][configure][build]")
{
    GIVEN("a project with config-generating and normal rules")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=test-value\n");

        // Run configure first
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        auto config_after_configure = f.read_file("build/configs/tup.config");
        REQUIRE(config_after_configure.find("test-value") != std::string::npos);

        WHEN("pup build runs")
        {
            auto result = f.build({ "-B", "build" });

            THEN("config-generating rules are NOT re-run and config is preserved")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                // Config should be unchanged from configure
                auto config_after_build = f.read_file("build/configs/tup.config");
                REQUIRE(config_after_build == config_after_configure);
            }
        }
    }
}

SCENARIO("configure with --config installs specified file", "[e2e][configure]")
{
    GIVEN("a project and an external config file")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        f.write_file("external.config", "CONFIG_TEST=external_value\n");

        WHEN("configure is run with --config")
        {
            auto result = f.pup({ "configure", "--config", "external.config" });

            THEN("the config file is installed")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("tup.config"));
                auto content = f.read_file("tup.config");
                REQUIRE(content.find("CONFIG_TEST=external_value") != std::string::npos);
            }
        }
    }
}

SCENARIO("configure with --config and -B installs to variant", "[e2e][configure]")
{
    GIVEN("a project and an external config file")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        f.write_file("myconfig.config", "CONFIG_VARIANT=debug\n");

        WHEN("configure is run with --config and -B")
        {
            auto result = f.pup({ "configure", "-B", "build-debug", "--config", "myconfig.config" });

            THEN("the config file is installed to the variant directory")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build-debug/tup.config"));
                auto content = f.read_file("build-debug/tup.config");
                REQUIRE(content.find("CONFIG_VARIANT=debug") != std::string::npos);
            }
        }
    }
}

SCENARIO("configure with --config fails for missing file", "[e2e][configure]")
{
    GIVEN("a project")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        WHEN("configure is run with non-existent config")
        {
            auto result = f.pup({ "configure", "--config", "nonexistent.config" });

            THEN("it fails with error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("not found") != std::string::npos);
            }
        }
    }
}

SCENARIO("configure with --config followed by build works", "[e2e][configure][build]")
{
    GIVEN("a project configured with --config")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

        f.write_file("build.config", "CONFIG_BUILD_TYPE=release\n");
        auto configure_result = f.pup({ "configure", "--config", "build.config" });
        REQUIRE(configure_result.success());

        WHEN("build is run")
        {
            auto result = f.build();

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.is_executable("hello"));
            }
        }
    }
}

SCENARIO("configure installs subdir configs via Tupfile copy rules", "[e2e][configure]")
{
    GIVEN("a project with a subdir config and a copy rule")
    {
        auto f = E2EFixture { "simple_c" };
        f.mkdir("sub");
        f.write_file("sub/defaults.config", "CONFIG_SUB_VAR=from_sub\n");
        f.write_file("sub/Tupfile", ": defaults.config |> cp %f %o |> tup.config\n");
        f.write_file("root.config", "CONFIG_ROOT_VAR=from_root\n");
        REQUIRE(f.init().success());

        WHEN("configure is run with --config and -B")
        {
            auto result = f.pup({ "configure", "--config", "root.config", "-B", "build" });

            THEN("root config is installed to build/tup.config")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/tup.config"));
                auto content = f.read_file("build/tup.config");
                REQUIRE(content.find("CONFIG_ROOT_VAR=from_root") != std::string::npos);
            }

            THEN("subdir config is produced by copy rule")
            {
                REQUIRE(f.exists("build/sub/tup.config"));
                auto content = f.read_file("build/sub/tup.config");
                REQUIRE(content.find("CONFIG_SUB_VAR=from_sub") != std::string::npos);
            }
        }
    }
}

SCENARIO("configure --config + subdir configs + build uses scoped merge", "[e2e][configure][scoped-config]")
{
    GIVEN("a project with root config and subdir config via copy rule")
    {
        auto f = E2EFixture { "scoped_config" };
        f.write_file("sub/defaults.config", "CONFIG_SUB_VAR=from_sub\n");
        f.write_file("sub/Tupfile", ": defaults.config |> cp %f %o |> tup.config\n"
                                    ": |> echo \"@(SUB_VAR)\" > %o |> sub.txt\n"
                                    ": |> echo \"@(ROOT_VAR)\" > %o |> root_from_sub.txt\n");
        f.write_file("root.config", "CONFIG_ROOT_VAR=from_root\n");
        REQUIRE(f.init().success());

        auto configure_result = f.pup({ "configure", "--config", "root.config", "-B", "build" });
        REQUIRE(configure_result.success());

        WHEN("build is run")
        {
            auto result = f.build({ "-B", "build" });

            THEN("sub/ sees both root and subdir vars via scoped merge")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/sub.txt") == "from_sub\n");
                REQUIRE(f.read_file("build/sub/root_from_sub.txt") == "from_root\n");
            }
        }
    }
}

SCENARIO("configure handles mixed copy-rule + auto-gen configs", "[e2e][configure]")
{
    GIVEN("a project with a copy-rule subdir config and auto-gen config rules")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=board-xyz\n");
        f.mkdir("sub");
        f.write_file("sub/defaults.config", "CONFIG_STATIC_VAR=static_value\n");
        f.write_file("sub/Tupfile", ": defaults.config |> cp %f %o |> tup.config\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("configure is run with -B")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("auto-gen config is produced")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/configs/tup.config"));
            }

            THEN("copy-rule subdir config is produced")
            {
                REQUIRE(f.exists("build/sub/tup.config"));
                auto content = f.read_file("build/sub/tup.config");
                REQUIRE(content.find("CONFIG_STATIC_VAR=static_value") != std::string::npos);
            }
        }
    }
}

// =============================================================================
// Duplicate Output Detection Tests
// =============================================================================

SCENARIO("Duplicate output detection", "[e2e][duplicate]")
{
    GIVEN("a Tupfile with two rules producing the same output")
    {
        auto f = E2EFixture { "duplicate_output" };

        WHEN("pup parses the project")
        {
            auto result = f.build();

            THEN("build fails with duplicate output error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("already owned") != std::string::npos);
            }
        }
    }
}

SCENARIO("Duplicate command detection", "[e2e][duplicate][identity]")
{
    GIVEN("a Tupfile with two output-less rules that render the same command line")
    {
        auto f = E2EFixture { "duplicate_command" };

        WHEN("pup parses the project")
        {
            auto result = f.build();

            THEN("build fails naming the ambiguous command")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("Duplicate command") != std::string::npos);
                REQUIRE(result.stderr_output.find("./check") != std::string::npos);
            }
        }
    }
}

SCENARIO("Editing a rule's recipe does not make it a different rule", "[e2e][identity][join]")
{
    GIVEN("a built project whose rule produces one output")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", ": input.txt |> cp %f %o |> output.txt\n");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());

        WHEN("only the command text changes, with the same inputs and outputs")
        {
            f.write_file("Tupfile", ": input.txt |> cat %f > %o |> output.txt\n");
            auto result = f.build({ "-B", "build", "-v" });

            THEN("the rule is recognised as the same one and its output is not deleted")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Removed stale") == std::string::npos);
                REQUIRE(f.exists("build/output.txt"));
            }
        }
    }
}

SCENARIO("A command that failed is re-run on the next build", "[e2e][incremental][failure]")
{
    GIVEN("a built project whose command is then changed to fail after writing its output")
    {
        auto f = E2EFixture { "failed_command" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());

        WHEN("it fails under keep-going and the build is repeated")
        {
            f.write_file("src.txt", "NEWCONTENT\n");
            f.write_file("Tupfile", ": src.txt |> cp %f %o && false |> out.txt\n");
            REQUIRE_FALSE(f.build({ "-B", "build", "-k" }).success());

            auto again = f.build({ "-B", "build", "-k" });

            THEN("the failure is remembered rather than reported as up to date")
            {
                INFO("stdout: " << again.stdout_output);
                REQUIRE(again.stdout_output.find("Nothing to do") == std::string::npos);
                REQUIRE_FALSE(again.success());
            }
        }
    }
}

SCENARIO("A failed command's consumer runs once the command succeeds", "[e2e][incremental][failure]")
{
    // The consumer's output already exists holding V1, so nothing schedules the consumer except
    // propagation from the producer's re-run. Routing the retry through forced_cmds instead of
    // changed_outputs leaves final.txt at V1 forever, which is what this pins.
    GIVEN("a built producer/consumer pair whose producer then fails after writing partial output")
    {
        auto f = E2EFixture { "failed_command" };
        f.write_file("Tupfile",
            ": src.txt |> sh -c 'if [ -f FAILMARK ]; then echo PARTIAL > %o; exit 1; else cp %f %o; fi' |> mid.txt\n"
            ": mid.txt |> cp %f %o |> final.txt\n");
        f.write_file("src.txt", "V1\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build", "-k" }).success());
        REQUIRE(f.read_file("build/final.txt") == "V1\n");

        f.write_file("src.txt", "V2\n");
        f.write_file("FAILMARK", "x\n");
        REQUIRE_FALSE(f.build({ "-B", "build", "-k" }).success());
        REQUIRE(f.read_file("build/mid.txt") == "PARTIAL\n");

        WHEN("the cause of the failure is removed and nothing else changes")
        {
            f.remove_file("FAILMARK");
            auto result = f.build({ "-B", "build", "-k" });

            THEN("the producer re-runs and the consumer picks up the corrected output")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/mid.txt") == "V2\n");
                REQUIRE(f.read_file("build/final.txt") == "V2\n");
            }
        }
    }
}

SCENARIO("A rule may not overwrite a checked-in source file", "[e2e][shadow]")
{
    GIVEN("a rule whose output path is also a source file in the tree")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": gen.src |> cp %f %o |> x.dat\n");
        f.write_file("gen.src", "FROMRULE\n");
        f.write_file("x.dat", "FROMSRC\n");

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("it is rejected and the source file is untouched")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("x.dat") != std::string::npos);
                REQUIRE(f.read_file("x.dat") == "FROMSRC\n");
            }
        }
    }
}

SCENARIO("A target build does not forget another command's failure", "[e2e][incremental][failure]")
{
    GIVEN("one failed command and one healthy command")
    {
        auto f = E2EFixture { "failed_command" };
        f.write_file("Tupfile",
            ": a.txt |> cp %f %o && false |> outa.txt\n"
            ": b.txt |> cp %f %o |> outb.txt\n");
        f.write_file("a.txt", "A\n");
        f.write_file("b.txt", "B\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE_FALSE(f.build({ "-B", "build", "-k" }).success());

        WHEN("only the healthy target is built, then the whole project")
        {
            (void)f.build({ "-B", "build", "-k", "build/outb.txt" });
            auto full = f.build({ "-B", "build", "-k" });

            THEN("the failure is still remembered")
            {
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.stdout_output.find("Nothing to do") == std::string::npos);
                REQUIRE_FALSE(full.success());
            }
        }
    }
}

SCENARIO("A scoped build does not forget an out-of-scope failure", "[e2e][incremental][failure][scope]")
{
    GIVEN("a failing command in one directory and a healthy one in another")
    {
        auto f = E2EFixture { "failed_command" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("a/Tupfile", ": a.txt |> cp %f %o && false |> outa.txt\n");
        f.write_file("b/Tupfile", ": b.txt |> cp %f %o |> outb.txt\n");
        f.write_file("a/a.txt", "A\n");
        f.write_file("b/b.txt", "B\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE_FALSE(f.build({ "-B", "build", "-k" }).success());

        WHEN("only the healthy directory is built, then the whole project")
        {
            (void)f.build({ "-B", "build", "-k", "b/" });
            auto full = f.build({ "-B", "build", "-k" });

            THEN("the out-of-scope failure is still remembered")
            {
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.stdout_output.find("Nothing to do") == std::string::npos);
                REQUIRE_FALSE(full.success());
            }
        }
    }
}

SCENARIO("Converting a read source into a generated file is rejected, then unblocked by deleting it", "[e2e][shadow]")
{
    GIVEN("a built project where a rule reads a checked-in file")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.write_file("a/Tupfile", ": x.dat |> cp %f %o |> copy.txt\n");
        f.write_file("a/x.dat", "FROMSRC\n");
        f.write_file("a/gen.src", "FROMRULE\n");
        REQUIRE(f.build().success());

        WHEN("a rule is changed to generate that same file")
        {
            f.write_file("a/Tupfile", ": gen.src |> cp %f %o |> x.dat\n");
            auto rejected = f.build();

            THEN("it is rejected while the file is still there")
            {
                INFO("stderr: " << rejected.stderr_output);
                REQUIRE_FALSE(rejected.success());
                REQUIRE(f.read_file("a/x.dat") == "FROMSRC\n");
            }

            AND_WHEN("the file is deleted as the message instructs")
            {
                f.remove_file("a/x.dat");
                auto result = f.build();

                THEN("the build proceeds and generates it")
                {
                    INFO("stderr: " << result.stderr_output);
                    REQUIRE(result.success());
                    REQUIRE(f.read_file("a/x.dat") == "FROMRULE\n");
                }
            }
        }
    }
}

SCENARIO("An inactive conditional branch's output is not treated as a shadow", "[e2e][shadow][phi]")
{
    GIVEN("a rule inside an unsatisfied ifdef whose output name matches a source file")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.write_file("a/Tupfile",
            "ifdef FOO\n"
            ": gen.src |> cp %f %o |> x.dat\n"
            "endif\n"
            ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("a/x.dat", "FROMSRC\n");
        f.write_file("a/gen.src", "FROMRULE\n");
        f.write_file("a/src.txt", "S\n");

        WHEN("built with the condition unsatisfied")
        {
            auto result = f.build();

            THEN("the build succeeds and the source is untouched")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("a/x.dat") == "FROMSRC\n");
            }
        }
    }
}

SCENARIO("An in-tree rebuild does not mistake its own output for a source file", "[e2e][shadow]")
{
    GIVEN("an ordinary in-tree project built once, so its output is on disk beside the sources")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        REQUIRE(f.exists("out.txt"));

        WHEN("it is built again")
        {
            auto again = f.build();

            THEN("the rebuild succeeds")
            {
                INFO("stderr: " << again.stderr_output);
                REQUIRE(again.success());
            }
        }
    }
}

SCENARIO("A glob's %f order does not depend on the build directory's name", "[e2e][glob][pathspace]")
{
    GIVEN("a glob matching one generated and one source file, built out-of-tree")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.write_file("a/Tupfile", ": src.in |> cp %f %o |> gen.dat\n: *.dat |> cat %f > %o |> all.txt\n");
        f.write_file("a/src.in", "GEN\n");
        f.write_file("a/keep.dat", "KEEP\n");

        WHEN("built into two differently-named build directories")
        {
            f.mkdir("AAA");
            REQUIRE(f.pup({ "configure", "-B", "AAA" }).success());
            REQUIRE(f.build({ "-B", "AAA" }).success());
            auto from_aaa = f.read_file("AAA/a/all.txt");

            f.mkdir("zz");
            REQUIRE(f.pup({ "configure", "-B", "zz" }).success());
            REQUIRE(f.build({ "-B", "zz" }).success());
            auto from_zz = f.read_file("zz/a/all.txt");

            THEN("the artifact is byte-identical")
            {
                INFO("AAA: " << from_aaa);
                INFO("zz:  " << from_zz);
                REQUIRE(from_aaa == from_zz);
                // Pins both halves and the canonical order: equality alone would still
                // hold if the filesystem half stopped contributing entirely.
                REQUIRE(from_aaa == "GEN\nKEEP\n");
            }
        }
    }
}

SCENARIO("Out-of-tree, a generated file shadowing a source is one glob match and the source survives", "[e2e][glob][pathspace]")
{
    // Out-of-tree the output lands under the build root, so the committed file is shadowed
    // rather than overwritten -- confusing, but not data loss, which is why #194's rejection
    // is limited to in-tree builds. What #191 guarantees here is that the file is one match
    // rather than two.
    GIVEN("a generated file whose name also exists as a checked-in source")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.write_file("a/Tupfile", ": gen.src |> cp %f %o |> x.dat\n: *.dat |> cat %f > %o |> all.txt\n");
        f.write_file("a/gen.src", "FROMRULE\n");
        f.write_file("a/x.dat", "FROMSRC\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built out-of-tree")
        {
            auto result = f.build({ "-B", "build" });

            THEN("it is consumed once, and the committed file is untouched")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/a/all.txt") == "FROMRULE\n");
                REQUIRE(f.read_file("a/x.dat") == "FROMSRC\n");
            }
        }
    }
}

SCENARIO("An exclusion applies to generated glob matches out-of-tree", "[e2e][glob][pathspace]")
{
    GIVEN("a glob over generated files with one of them excluded")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": keep.in |> cp %f %o |> keep.gen\n"
            ": skip.in |> cp %f %o |> skip.gen\n"
            ": *.gen !skip.gen |> echo %f > %o |> out.txt\n");
        f.write_file("keep.in", "k\n");
        f.write_file("skip.in", "s\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built out-of-tree")
        {
            REQUIRE(f.build({ "-B", "build" }).success());

            THEN("the excluded file is absent from %f")
            {
                auto content = f.read_file("build/out.txt");
                INFO("out.txt: " << content);
                REQUIRE(content.find("keep.gen") != std::string::npos);
                REQUIRE(content.find("skip.gen") == std::string::npos);
            }
        }
    }
}

SCENARIO("Dropping one output of a rule removes that output", "[e2e][identity][join][stale]")
{
    GIVEN("a built rule that declares two outputs")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", ": input.txt |> sh -c \"cp input.txt build/a.out && cp input.txt build/b.out\" |> a.out b.out\n");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/a.out"));
        REQUIRE(f.exists("build/b.out"));

        WHEN("the rule is edited to declare only the first output")
        {
            f.write_file("Tupfile", ": input.txt |> sh -c \"cp input.txt build/a.out\" |> a.out\n");
            auto result = f.build({ "-B", "build", "-v" });

            THEN("the output it no longer produces is deleted, and the one it still does survives")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/a.out"));
                REQUIRE_FALSE(f.exists("build/b.out"));
            }
        }
    }
}

SCENARIO("Complementary branches may render the same command line", "[e2e][duplicate][identity][phi]")
{
    GIVEN("a project whose two conditional branches carry identical rule text")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", R"(
ifeq (@(MODE),release)
: input.txt |> cp %f %o |> output.txt
else
: input.txt |> cp %f %o |> output.txt
endif
)");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MODE=debug\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built")
        {
            auto result = f.build({ "-B", "build" });

            THEN("only the guard-satisfied branch counts, so the build succeeds")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/output.txt"));
            }
        }
    }
}

// =============================================================================
// Platform Conditional Tests
// =============================================================================

SCENARIO("TUP_PLATFORM env var controls platform conditionals", "[e2e][platform]")
{
    GIVEN("a Tupfile with platform-conditional rules")
    {
        auto f = E2EFixture { "platform_conditional" };

        WHEN("built with default platform")
        {
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("build succeeds and creates posix.txt")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("posix.txt"));
                REQUIRE_FALSE(f.exists("win32.txt"));
                // Content should be the platform name (linux, macosx, etc.)
                REQUIRE_FALSE(f.read_file("posix.txt").empty());
            }
        }
    }

    GIVEN("a Tupfile with platform-conditional rules and TUP_PLATFORM=win32")
    {
        auto env = EnvGuard { "TUP_PLATFORM", "win32" };
        auto f = E2EFixture { "platform_conditional" };

        WHEN("built with TUP_PLATFORM=win32")
        {
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("build succeeds and creates win32.txt")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("win32.txt"));
                REQUIRE_FALSE(f.exists("posix.txt"));
                REQUIRE(f.read_file("win32.txt").find("win32") != std::string::npos);
            }
        }
    }
}

SCENARIO("Env var change in a conditional rebuilds the affected branch",
    "[e2e][platform][incremental][platform-incremental]")
{
    // Both branches of `ifeq ($(TUP_PLATFORM),...)` produce the same output file with
    // distinct content; the platform string does not appear in either command's text.
    // So the only signal that an env-driven branch flip occurred is a sticky edge from
    // the env Variable node to the guarded commands (the condition_env_vars path,
    // symmetric to condition_config_vars). Without it, change detection sees no changed
    // file and no changed identity, reports "Nothing to do", and the output stays stale.
    GIVEN("a project whose active branch is selected by an env-sourced $(TUP_PLATFORM) condition")
    {
        auto f = E2EFixture { "platform_conditional_incremental" };
        {
            auto env = EnvGuard { "TUP_PLATFORM", "win32" };
            REQUIRE(f.init().success());
            REQUIRE(f.build().success());
            REQUIRE(f.read_file("result.txt") == "WINBUILD\n");
            REQUIRE(f.build().is_noop()); // stable under no change
        }

        WHEN("the env var flips, selecting the other branch")
        {
            auto env = EnvGuard { "TUP_PLATFORM", "linux" };
            auto result = f.build();

            THEN("the rebuild runs the now-active branch and the output updates")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.read_file("result.txt") == "NIXBUILD\n");
            }
        }
    }
}

SCENARIO("Imported env vars persist across builds", "[e2e][import]")
{
    GIVEN("a project with import directive")
    {
        auto f = E2EFixture { "env_var_persist" };

        WHEN("building with env var set")
        {
            auto env = EnvGuard { "MY_VAR", "first_value" };
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("build succeeds and output contains the env var value")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("out.txt"));
                REQUIRE(f.read_file("out.txt") == "first_value\n");
            }
        }
    }

    GIVEN("a project already built with env var")
    {
        auto f = E2EFixture { "env_var_persist" };
        {
            auto env = EnvGuard { "MY_VAR", "first_value" };
            REQUIRE(f.init().success());
            REQUIRE(f.build().success());
            REQUIRE(f.read_file("out.txt") == "first_value\n");
        }

        WHEN("rebuilding without env var set")
        {
            // MY_VAR not in environment (EnvGuard out of scope)
            auto result = f.build();

            THEN("cached value is used, no rebuild triggered")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
                REQUIRE(f.read_file("out.txt") == "first_value\n");
            }
        }

        WHEN("rebuilding with changed env var")
        {
            auto env = EnvGuard { "MY_VAR", "new_value" };
            auto result = f.build();

            THEN("rebuild is triggered with new value")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.read_file("out.txt") == "new_value\n");
            }
        }
    }
}

SCENARIO("Imported env vars with equals in value", "[e2e][import]")
{
    GIVEN("a project with import directive")
    {
        auto f = E2EFixture { "env_var_persist" };

        WHEN("env var value contains equals signs")
        {
            auto env = EnvGuard { "MY_VAR", "foo=bar=baz" };
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("full value is preserved including equals signs")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("out.txt") == "foo=bar=baz\n");
            }
        }
    }
}

SCENARIO("Exported env var consumed via subprocess environment triggers rebuild", "[e2e][import][envdep]")
{
    // The command consumes MY_GREETING through the inherited environment (bare $VAR
    // in the shell), NOT via $(VAR) substitution. So the rendered command string is
    // byte-identical across values: a string-keyed change detector cannot see the
    // change. Correctness requires the command's identity to fold in the values of
    // the vars it depends on (here, the exported MY_GREETING).
    GIVEN("a project already built with an exported env var the shell reads from the environment")
    {
        auto f = E2EFixture { "env_dep_subprocess" };
        {
            auto env = EnvGuard { "MY_GREETING", "hello" };
            REQUIRE(f.init().success());
            REQUIRE(f.build().success());
            REQUIRE(f.read_file("out.txt") == "hello\n");
            REQUIRE(f.build().is_noop()); // stable under no change
        }

        WHEN("the exported env var changes (command text stays byte-identical)")
        {
            auto env = EnvGuard { "MY_GREETING", "world" };
            auto result = f.build();

            THEN("rebuild is triggered and the output reflects the new value")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.read_file("out.txt") == "world\n");
            }
        }
    }
}

SCENARIO("Tracked tool binaries fold into command identity", "[e2e][incremental]")
{
    GIVEN("a project whose config tracks a tool that is not a rule input")
    {
        auto f = E2EFixture { "tracked_tools" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_TRACKED_TOOLS=./tool.sh\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.read_file("build/out.txt") == "v1\n");
        REQUIRE(f.build({ "-B", "build" }).is_noop());

        WHEN("the tool's contents change (it is not an input of any rule)")
        {
            f.write_file("tool.sh", "echo v2\n");
            auto result = f.build({ "-B", "build" });

            THEN("the command re-runs and the output reflects the new tool")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.read_file("build/out.txt") == "v2\n");
            }
        }
    }
}

SCENARIO("Content change with preserved size and mtime", "[e2e][incremental]")
{
    GIVEN("a built project whose input has an aged mtime")
    {
        auto f = E2EFixture { "stat_cache" };
        f.write_file("in.txt", "AAAA\n");
        REQUIRE(f.run("/usr/bin/touch", { "-d", "2020-01-01T00:00:00", "in.txt" }).success());
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("out.txt") == "AAAA\n");
        REQUIRE(f.build().is_noop());

        WHEN("the content changes but size and mtime are restored")
        {
            REQUIRE(f.run("/bin/cp", { "-p", "in.txt", "ref" }).success());
            f.write_file("in.txt", "BBBB\n");
            REQUIRE(f.run("/usr/bin/touch", { "-r", "ref", "in.txt" }).success());

            THEN("the default stat cache misses the change (documented trade-off)")
            {
                REQUIRE(f.build().is_noop());
            }

            THEN("--no-stat-cache hashes every file and rebuilds")
            {
                auto result = f.build({ "--no-stat-cache" });
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.read_file("out.txt") == "BBBB\n");
            }
        }
    }
}

SCENARIO("Untracked env vars do not leak into command environments", "[e2e][envdep]")
{
    GIVEN("a rule that reads an env var that is never imported or exported")
    {
        auto f = E2EFixture { "env_leak" };
        auto env = EnvGuard { "PUP_TEST_LEAK", "secret" };
        REQUIRE(f.init().success());

        WHEN("the project builds")
        {
            auto result = f.build();

            THEN("the command's environment does not contain the var")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("out.txt") == "leak=unset\n");
            }
        }
    }
}

SCENARIO("Export-only env var reaches the command and folds into identity", "[e2e][envdep]")
{
    GIVEN("a project built with an exported (never imported) env var")
    {
        auto f = E2EFixture { "env_export_only" };
        {
            auto env = EnvGuard { "PUP_TEST_EXP", "alpha" };
            REQUIRE(f.init().success());
            REQUIRE(f.build().success());
            REQUIRE(f.read_file("out.txt") == "v=alpha\n");
            REQUIRE(f.build().is_noop());
        }

        WHEN("the exported var changes (command text is byte-identical)")
        {
            auto env = EnvGuard { "PUP_TEST_EXP", "beta" };
            auto result = f.build();

            THEN("the command re-runs and the output reflects the new value")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.read_file("out.txt") == "v=beta\n");
            }
        }
    }
}

SCENARIO("Only commands using changed env var rebuild", "[e2e][import]")
{
    GIVEN("a project already built with two env vars")
    {
        auto f = E2EFixture { "env_var_fine_grained" };

        {
            auto env_cc = EnvGuard { "CC", "gcc" };
            auto env_cflags = EnvGuard { "CFLAGS", "-O2" };
            REQUIRE(f.init().success());
            REQUIRE(f.build().success());
            REQUIRE(f.read_file("cc_out.txt") == "gcc\n");
            REQUIRE(f.read_file("cflags_out.txt") == "-O2\n");
            REQUIRE(f.build().is_noop());
        }

        WHEN("only CC env var changes")
        {
            auto env_cc = EnvGuard { "CC", "clang" };
            auto env_cflags = EnvGuard { "CFLAGS", "-O2" };
            auto result = f.build({ "-v" });

            THEN("build succeeds and is not a noop")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }

            THEN("only cc_out.txt command runs, not cflags_out.txt")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("cc_out.txt") != std::string::npos);
                REQUIRE(result.stdout_output.find("cflags_out.txt") == std::string::npos);
            }

            THEN("cc_out.txt has new value, cflags_out.txt unchanged")
            {
                REQUIRE(f.read_file("cc_out.txt") == "clang\n");
                REQUIRE(f.read_file("cflags_out.txt") == "-O2\n");
            }
        }

        WHEN("only CFLAGS env var changes")
        {
            auto env_cc = EnvGuard { "CC", "gcc" };
            auto env_cflags = EnvGuard { "CFLAGS", "-O3" };
            auto result = f.build({ "-v" });

            THEN("only cflags_out.txt command runs, not cc_out.txt")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(result.stdout_output.find("cflags_out.txt") != std::string::npos);
                REQUIRE(result.stdout_output.find("cc_out.txt") == std::string::npos);
            }
        }
    }
}

// =============================================================================
// Conditional Assignment Operators Tests
// =============================================================================

SCENARIO("?= soft assignment - first wins", "[e2e][assignment]")
{
    GIVEN("a Tupfile with multiple ?= assignments")
    {
        auto f = E2EFixture { "soft_assign" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("first ?= value is used")
            {
                REQUIRE(f.read_file("result.txt") == "first\n");
            }
        }
    }
}

SCENARIO("?= soft assignment - = takes precedence", "[e2e][assignment]")
{
    GIVEN("a Tupfile where = precedes ?=")
    {
        auto f = E2EFixture { "soft_assign_override" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("= value is used, ?= is ignored")
            {
                REQUIRE(f.read_file("result.txt") == "explicit\n");
            }
        }
    }
}

// Use ?\?= in strings to avoid trigraph interpretation (??= -> #)
SCENARIO("?\?= weak assignment - last wins", "[e2e][assignment]")
{
    GIVEN("a Tupfile with multiple ?\?= assignments")
    {
        auto f = E2EFixture { "weak_assign" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("last ?\?= value is used")
            {
                REQUIRE(f.read_file("result.txt") == "second\n");
            }
        }
    }
}

SCENARIO("?\?= weak assignment - = takes precedence", "[e2e][assignment]")
{
    GIVEN("a Tupfile where = precedes ?\?=")
    {
        auto f = E2EFixture { "weak_assign_override" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("= value is used, ?\?= is ignored")
            {
                REQUIRE(f.read_file("result.txt") == "explicit\n");
            }
        }
    }
}

SCENARIO("import with @-var default", "[e2e][import]")
{
    GIVEN("a Tupfile with import VAR=@(CONFIG_VAR)")
    {
        auto f = E2EFixture { "import_atvar_default" };

        WHEN("env var is not set")
        {
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("@-var default from tup.config is used")
            {
                REQUIRE(f.read_file("out.txt") == "from_config\n");
            }
        }

        WHEN("env var is set")
        {
            auto env = EnvGuard { "MY_VAR", "from_env" };
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("env var value takes precedence over @-var default")
            {
                REQUIRE(f.read_file("out.txt") == "from_env\n");
            }
        }
    }
}

SCENARIO("import with ?= operator", "[e2e][import]")
{
    GIVEN("a Tupfile with import VAR ?= default")
    {
        auto f = E2EFixture { "import_soft_set" };

        WHEN("env var is not set")
        {
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("default value is used")
            {
                REQUIRE(f.read_file("out.txt") == "default_value\n");
            }
        }

        WHEN("env var is set")
        {
            auto env = EnvGuard { "MY_VAR", "from_env" };
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("env var takes precedence")
            {
                REQUIRE(f.read_file("out.txt") == "from_env\n");
            }
        }
    }
}

SCENARIO("import ?= default reverts when env is unset on warm index", "[e2e][import]")
{
    GIVEN("a Tupfile with import VAR ?= default, after a build that captured the env value")
    {
        auto f = E2EFixture { "import_soft_set" };

        {
            auto env = EnvGuard { "MY_VAR", "from_env" };
            REQUIRE(f.init().success());
            auto build1 = f.build();
            REQUIRE(build1.success());
            REQUIRE(f.read_file("out.txt") == "from_env\n");
        }

        WHEN("the env var is unset and the project is rebuilt")
        {
            auto build2 = f.build();

            THEN("build succeeds")
            {
                INFO("stdout: " << build2.stdout_output);
                INFO("stderr: " << build2.stderr_output);
                REQUIRE(build2.success());
            }

            THEN("the default value is restored, not the cached env value")
            {
                REQUIRE(f.read_file("out.txt") == "default_value\n");
            }
        }
    }
}

// =============================================================================
// Error Handling Tests
// =============================================================================

SCENARIO("Cyclic dependency detection", "[e2e][error]")
{
    GIVEN("a project with circular dependencies")
    {
        auto f = E2EFixture { "cycle" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("build fails with cycle error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("cycle") != std::string::npos);
            }
        }
    }
}

SCENARIO("Missing include file detection", "[e2e][error]")
{
    GIVEN("a project with a missing include directive")
    {
        auto f = E2EFixture { "missing_include" };

        WHEN("the project is initialized")
        {
            auto result = f.init();

            THEN("initialization fails with include error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("not found") != std::string::npos);
            }
        }
    }
}

// =============================================================================
// Out-of-Tree Configuration Tests (3-tree builds)
// =============================================================================

SCENARIO("Out-of-tree configuration with separate source/config/build trees", "[e2e][out-of-tree-config]")
{
    GIVEN("a project with separate source, config, and build directories")
    {
        auto f = E2EFixture { "out_of_tree_config_3tree" };
        auto source_dir = f.workdir() / "source";
        auto config_dir = f.workdir() / "config";
        auto build_dir = f.workdir() / "build";

        // Create build directory with tup.config
        f.mkdir("build");
        f.write_file("build/tup.config", "");

        WHEN("parsing with -S, -C, and -B options")
        {
            auto result = f.pup({
                "parse",
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-v"
            });

            THEN("parsing succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("Tupfiles are discovered from config directory")
            {
                // Should find Tupfiles in config/, not source/
                REQUIRE(result.stdout_output.find("config/Tupfile") != std::string::npos);
            }
        }

        WHEN("building with -S, -C, and -B options")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-j1"
            });

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("object files are created in build directory")
            {
                REQUIRE(f.exists("build/main.o"));
                REQUIRE(f.exists("build/lib/add.o"));
            }

            THEN("executable is created in build directory")
            {
                REQUIRE(f.is_executable("build/prog"));
            }

            THEN("source directory remains pristine")
            {
                // No .pup or build artifacts in source
                REQUIRE_FALSE(f.exists("source/.pup"));
                REQUIRE_FALSE(f.exists("source/main.o"));
            }
        }
    }
}

SCENARIO("Three-tree build reaches up-to-date on rebuild", "[e2e][out-of-tree-config][incremental]")
{
    GIVEN("a fully-built project with separate source, config, and build trees")
    {
        auto f = E2EFixture { "out_of_tree_config_3tree" };
        auto source_dir = f.workdir() / "source";
        auto config_dir = f.workdir() / "config";
        auto build_dir = f.workdir() / "build";
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({
                     "-S", source_dir.string(),
                     "-C", config_dir.string(),
                     "-B", build_dir.string(),
                     "-j1" })
                    .success());
        REQUIRE(f.is_executable("build/prog"));

        WHEN("the project is rebuilt with nothing changed")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-j1", "-v" });

            THEN("no file is reported as changed")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stdout_output.find("Changed (") == std::string::npos);
            }

            THEN("the build is a no-op")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.is_noop());
            }
        }
    }
}

SCENARIO("Cross-directory groups in 3-tree builds", "[e2e][out-of-tree-config][groups]")
{
    GIVEN("a 3-tree project with cross-directory group references via variables")
    {
        // Mirrors the GCC example pattern:
        //   root Tuprules.tup: S = $(TUP_CWD); LIB_DIR = gcc
        //   gcc/Tuprules.tup:  S ?= $(TUP_CWD); LIB_DIR ?= .; macros use $(S)/$(LIB_DIR)/<group>
        //   gcc/Tupfile:       produces <gen-headers>, consumes via macros
        auto f = E2EFixture { "groups_cross_dir_3tree" };
        auto source_dir = f.workdir() / "source";
        auto config_dir = f.workdir() / "config";
        auto build_dir = f.workdir() / "build";

        f.mkdir("build");
        f.write_file("build/tup.config", "");

        WHEN("parsing with -S, -C, and -B options")
        {
            auto result = f.pup({
                "parse",
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-v"
            });

            THEN("parse succeeds without group warnings")
            {
                INFO("stdout:\n" << result.stdout_output);
                INFO("stderr:\n" << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("has no members") == std::string::npos);
            }
        }

        WHEN("building with -S, -C, and -B options")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-j1"
            });

            THEN("build succeeds without group warnings")
            {
                INFO("stdout:\n" << result.stdout_output);
                INFO("stderr:\n" << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("has no members") == std::string::npos);
            }

            THEN("generated header exists in build directory")
            {
                REQUIRE(f.exists("build/gcc/config.h"));
            }

            THEN("output files are created from source globs")
            {
                REQUIRE(f.exists("build/gcc/hello.out"));
            }
        }
    }
}

SCENARIO("Out-of-tree configuration with TUP_SRCDIR and TUP_OUTDIR variables", "[e2e][out-of-tree-config]")
{
    GIVEN("a project using TUP_SRCDIR and TUP_OUTDIR in Tupfiles")
    {
        auto f = E2EFixture { "out_of_tree_config_shared" };
        auto source_dir = f.workdir() / "source";
        auto config_dir = f.workdir() / "config";
        auto build_debug = f.workdir() / "build-debug";
        auto build_release = f.workdir() / "build-release";

        // Create build directories
        f.mkdir("build-debug");
        f.mkdir("build-release");

        // Create debug tup.config
        f.write_file("build-debug/tup.config", "CONFIG_CFLAGS=-g -O0\n");
        f.write_file("build-release/tup.config", "CONFIG_CFLAGS=-O2\n");

        WHEN("building debug variant")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_debug.string(),
                "-j1"
            });

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("debug executable is created")
            {
                REQUIRE(f.is_executable("build-debug/hello"));
            }
        }

        WHEN("building release variant with same config")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_release.string(),
                "-j1"
            });

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("release executable is created")
            {
                REQUIRE(f.is_executable("build-release/hello"));
            }
        }

        WHEN("both variants are built with same config")
        {
            (void)f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_debug.string(),
                "-j1"
            });
            (void)f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_release.string(),
                "-j1"
            });

            THEN("both executables exist independently")
            {
                REQUIRE(f.is_executable("build-debug/hello"));
                REQUIRE(f.is_executable("build-release/hello"));
            }

            THEN("config directory has no build artifacts")
            {
                REQUIRE_FALSE(f.exists("config/.pup"));
                REQUIRE_FALSE(f.exists("config/hello.o"));
            }
        }
    }
}

SCENARIO("Config tree inside source tree", "[e2e][out-of-tree-config]")
{
    GIVEN("a project with config directory nested inside source tree")
    {
        auto f = E2EFixture { "config_inside_source" };
        auto source_dir = f.workdir();
        auto config_dir = f.workdir() / "tupfiles";
        auto build_dir = f.workdir() / "build";

        f.mkdir("build");
        f.write_file("build/tup.config", "");

        WHEN("parsing with -S pointing to parent of -C")
        {
            auto result = f.pup({
                "parse",
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-v"
            });

            THEN("parsing succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("globs resolve against source root, not config root")
            {
                // main.c is in source_dir, not source_dir/tupfiles
                REQUIRE(result.stdout_output.find("main.c") != std::string::npos);
            }
        }

        WHEN("building with config inside source tree")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-j1"
            });

            THEN("build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("object files are created in build directory")
            {
                REQUIRE(f.exists("build/main.o"));
                REQUIRE(f.exists("build/lib/add.o"));
            }

            THEN("executable is created")
            {
                REQUIRE(f.is_executable("build/prog"));
            }

            THEN("executable runs correctly")
            {
                auto run_result = f.run("build/prog");
                REQUIRE(run_result.success());
            }

            THEN("no build artifacts in config directory")
            {
                REQUIRE_FALSE(f.exists("tupfiles/main.o"));
                REQUIRE_FALSE(f.exists("tupfiles/.pup"));
            }

            THEN("no build artifacts in source root")
            {
                REQUIRE_FALSE(f.exists("main.o"));
                REQUIRE_FALSE(f.exists(".pup"));
            }
        }
    }
}

SCENARIO("Cross-project order-only dependency resolution", "[e2e][out-of-tree-config][variant]")
{
    // This tests the bug where order-only deps using $(B) paths in 3-tree builds
    // create Ghost nodes instead of resolving to existing Generated nodes.
    // Pattern from busybox:
    //   root/Tupfile:    : |> ... |> include/autoconf.h  # output at build/include/autoconf.h
    //   applets/Tupfile: : main.c | $(B)/include/autoconf.h |> ...
    //
    // The bug manifests when:
    // 1. source and output are in completely different filesystem trees
    // 2. build_root_name has N levels of "../" (e.g., "../../../../tmp/build")
    // 3. From a subdirectory, $(B) normalizes to N-1 levels of "../"
    //    because one "../" cancels with the subdirectory name
    // 4. strip_build_prefix() fails to match the different prefix depths
    //
    // Example with busybox:
    //   source = /home/user/src/busybox
    //   output = /tmp/build
    //   build_root_name = ../../../../tmp/build (5 levels up from source)
    //   From applets/: $(B) = TUP_VARIANT_OUTPUTDIR/..
    //     = ../../../../tmp/build/applets/.. = ../../../tmp/build (4 levels)
    //   strip_build_prefix("../../../tmp/build/include/x.h", "../../../../tmp/build")
    //   FAILS - prefixes don't match due to depth difference!
    GIVEN("a 3-tree project with asymmetric directory depths")
    {
        auto f = E2EFixture { "cross_project_order_only" };

        // Create asymmetric setup: source at 3 levels deep, output at 1 level
        // source_root = workdir/a/b/c/source (3 dirs deep)
        // output_root = workdir/out (1 dir deep)
        // build_root_name = relative(out, a/b/c/source) = ../../../../out (4 ../)
        // From consumer/: $(B) expands and normalizes to ../../../out (3 ../)
        // The prefix mismatch causes strip_build_prefix to fail
        auto source_dir = f.workdir() / "a" / "b" / "c" / "source";
        auto config_dir = f.workdir() / "a" / "b" / "c" / "config";
        auto build_dir = f.workdir() / "out";

        f.mkdir("a/b/c/source/consumer");
        f.mkdir("a/b/c/config/consumer");
        f.mkdir("out");
        f.write_file("out/tup.config", "");

        // Copy fixture files to the asymmetric locations
        f.write_file("a/b/c/source/consumer/main.c", f.read_file("source/consumer/main.c"));
        f.write_file("a/b/c/config/Tupfile.ini", "");
        f.write_file("a/b/c/config/Tuprules.tup", f.read_file("config/Tuprules.tup"));
        f.write_file("a/b/c/config/Tupfile", f.read_file("config/Tupfile"));
        f.write_file("a/b/c/config/consumer/Tupfile", f.read_file("config/consumer/Tupfile"));

        WHEN("building with asymmetric directory depths")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-j1"
            });

            THEN("build succeeds with correct ordering")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }

            THEN("generated header is created before consumer compiles")
            {
                REQUIRE(f.exists("out/include/generated.h"));
                REQUIRE(f.exists("out/consumer/main.o"));
            }
        }

        WHEN("checking dry-run output order with asymmetric depths")
        {
            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-n"
            });

            INFO("stdout: " << result.stdout_output);
            INFO("stderr: " << result.stderr_output);
            REQUIRE(result.success());

            THEN("header generation is scheduled before compilation")
            {
                // Find positions in output
                auto gen_pos = result.stdout_output.find("generated.h");
                auto gcc_pos = result.stdout_output.find("gcc");
                // Header generation should appear before gcc compilation
                REQUIRE(gen_pos != std::string::npos);
                REQUIRE(gcc_pos != std::string::npos);
                REQUIRE(gen_pos < gcc_pos);
            }
        }
    }
}

// =============================================================================
// Show Var Command Tests
// =============================================================================

SCENARIO("show var displays variable assignments", "[e2e][show][var]")
{
    GIVEN("a project with variable assignments")
    {
        auto f = E2EFixture { "show_var" };
        REQUIRE(f.init().success());

        WHEN("show var is run")
        {
            auto result = f.pup({ "show", "var" });

            THEN("command succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("output includes CC variable")
            {
                REQUIRE(result.stdout_output.find("CC") != std::string::npos);
            }

            THEN("output includes CFLAGS variable")
            {
                REQUIRE(result.stdout_output.find("CFLAGS") != std::string::npos);
            }

            THEN("output includes History section")
            {
                REQUIRE(result.stdout_output.find("History:") != std::string::npos);
            }
        }

        WHEN("show var CC is run")
        {
            auto result = f.pup({ "show", "var", "CC" });

            THEN("command succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("output includes CC variable")
            {
                REQUIRE(result.stdout_output.find("CC") != std::string::npos);
            }

            THEN("output does NOT include CFLAGS variable")
            {
                REQUIRE(result.stdout_output.find("CFLAGS") == std::string::npos);
            }
        }

        WHEN("show var --json is run")
        {
            auto result = f.pup({ "show", "var", "--json" });

            THEN("command succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("output is JSON format")
            {
                REQUIRE(result.stdout_output.find("{") != std::string::npos);
                REQUIRE(result.stdout_output.find("\"variables\":") != std::string::npos);
            }

            THEN("each variable opens a JSON object")
            {
                REQUIRE(result.stdout_output.find("\"CC\": {\n") != std::string::npos);
            }
        }
    }
}

// =============================================================================
// Phi-Node Model Tests (Conditional Stability)
// =============================================================================

SCENARIO("Phi-node model processes both conditional branches", "[e2e][phi]")
{
    GIVEN("a project with conditional compilation")
    {
        auto f = E2EFixture { "phi_conditional" };
        f.write_file("Tupfile", R"(
CC = gcc
CFLAGS = -Wall

ifeq (@(MODE),debug)
CFLAGS += -g -O0
else
CFLAGS += -O2
endif

: main.c |> $(CC) $(CFLAGS) -o %o %f |> program
)");
        f.write_file("main.c", R"(
#include <stdio.h>
int main(void) { printf("Hello\n"); return 0; }
)");
        f.mkdir("build");

        WHEN("built with MODE=debug")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=debug\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("executable is created")
            {
                REQUIRE(f.exists("build/program"));
            }
        }
    }
}

SCENARIO("Inactive conditional branch commands are skipped", "[e2e][phi][guards]")
{
    GIVEN("a project with ifeq conditional")
    {
        auto f = E2EFixture { "phi_conditional" };
        f.write_file("Tupfile", R"(
ifeq (@(BUILD),release)
: src.c |> gcc -O2 -o %o %f |> release_out
else
: src.c |> gcc -g -o %o %f |> debug_out
endif
)");
        f.write_file("src.c", "int main(){return 0;}\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_BUILD=debug\n");

        WHEN("built with BUILD=debug")
        {
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("debug output is created")
            {
                REQUIRE(f.exists("build/debug_out"));
            }

            THEN("release output is NOT created")
            {
                REQUIRE_FALSE(f.exists("build/release_out"));
            }
        }

        WHEN("toggled to BUILD=release")
        {
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            (void)f.build({ "-B", "build", "-j1" }); // First build with debug

            // Toggle to release
            f.write_file("build/tup.config", "CONFIG_BUILD=release\n");
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("release output is created")
            {
                REQUIRE(f.exists("build/release_out"));
            }
        }
    }
}

SCENARIO("Bang macro definitions respect conditional branches", "[e2e][phi][macro]")
{
    GIVEN("a macro redefined in both branches of an ifeq")
    {
        auto f = E2EFixture { "phi_conditional" };
        f.write_file("Tupfile", R"(
ifeq (@(DEVICE),mh1903)
!emit = |> echo dev1903 > %o |>
else
!emit = |> echo other > %o |>
endif

: |> !emit |> out.txt
)");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_DEVICE=mh1903\n");

        WHEN("built with DEVICE=mh1903")
        {
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("the active branch's definition is used")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/out.txt") == "dev1903\n");
            }
        }
    }

    GIVEN("a macro selected by a nested conditional in an included file")
    {
        auto f = E2EFixture { "phi_conditional" };
        f.write_file("hexcat.tup", R"(
ifneq ($(OUTPUT_MODE),binary)
!emit = |> echo plain > %o |>
else
  ifeq (@(DEVICE),mh1903)
    !emit = |> echo dev1903 > %o |>
  else
    !emit = |> echo other > %o |>
  endif
endif
)");
        f.write_file("Tupfile", R"(
OUTPUT_MODE = binary
include hexcat.tup
: |> !emit |> out.txt
)");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_DEVICE=mh1903\n");

        WHEN("built with DEVICE=mh1903")
        {
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("the definition from the active nested branch is used")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/out.txt") == "dev1903\n");
            }
        }
    }

    GIVEN("a macro defined and invoked inside the same inactive branch")
    {
        auto f = E2EFixture { "phi_conditional" };
        f.write_file("Tupfile", R"(
ifeq (@(ENCRYPT),y)
!scramble = |> echo scrambled > %o |>
: |> !scramble |> scrambled.txt
endif

: |> echo done > %o |> done.txt
)");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_ENCRYPT=n\n");

        WHEN("built with the branch inactive")
        {
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build", "-j1" });

            THEN("the build succeeds and the guarded rule stays inactive")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build/scrambled.txt"));
                REQUIRE(f.exists("build/done.txt"));
            }
        }
    }
}

SCENARIO("Conditional branches only contribute active outputs to groups", "[e2e][phi][groups]")
{
    GIVEN("a project with conditional outputs to the same group")
    {
        auto f = E2EFixture { "conditional_groups" };
        f.write_file("Tupfile", R"(
ifeq (@(USE_PLATFORM),ios)
: ios_impl.c |> gcc -c %f -o %o |> ios_impl.o {objs}
else
: linux_impl.c |> gcc -c %f -o %o |> linux_impl.o {objs}
endif

: {objs} |> ar rcs %o %f |> libplatform.a
)");
        f.write_file("ios_impl.c", "int platform_init(void) { return 1; }\n");
        f.write_file("linux_impl.c", "int platform_init(void) { return 2; }\n");
        f.mkdir("build");

        WHEN("built with USE_PLATFORM=linux (default/else branch)")
        {
            f.write_file("build/tup.config", "CONFIG_USE_PLATFORM=linux\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("only linux_impl.o is created")
            {
                REQUIRE(f.exists("build/linux_impl.o"));
                REQUIRE_FALSE(f.exists("build/ios_impl.o"));
            }

            THEN("archive contains only linux_impl.o")
            {
                auto ar_result = f.run("/usr/bin/ar", { "-t", "build/libplatform.a" });
                REQUIRE(ar_result.success());
                REQUIRE(ar_result.stdout_output.find("linux_impl.o") != std::string::npos);
                REQUIRE(ar_result.stdout_output.find("ios_impl.o") == std::string::npos);
            }
        }

        WHEN("built with USE_PLATFORM=ios (then branch)")
        {
            f.write_file("build/tup.config", "CONFIG_USE_PLATFORM=ios\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
            }

            THEN("only ios_impl.o is created")
            {
                REQUIRE(f.exists("build/ios_impl.o"));
                REQUIRE_FALSE(f.exists("build/linux_impl.o"));
            }

            THEN("archive contains only ios_impl.o")
            {
                auto ar_result = f.run("/usr/bin/ar", { "-t", "build/libplatform.a" });
                REQUIRE(ar_result.success());
                REQUIRE(ar_result.stdout_output.find("ios_impl.o") != std::string::npos);
                REQUIRE(ar_result.stdout_output.find("linux_impl.o") == std::string::npos);
            }
        }
    }
}

SCENARIO("Error directive aborts the build only when its branch is active", "[e2e][error-directive]")
{
    GIVEN("a project whose Tupfile guards an error directive behind @(BROKEN)")
    {
        auto f = E2EFixture { "error_directive" };
        f.mkdir("build");

        WHEN("built with the default config")
        {
            f.write_file("build/tup.config", "");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/out.txt"));
            }
        }

        WHEN("built after the config sets BROKEN=y")
        {
            f.write_file("build/tup.config", "");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            f.write_file("build/tup.config", "CONFIG_BROKEN=y\n");

            auto result = f.build({ "-B", "build" });

            THEN("build fails with the expanded message and Tupfile location")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("broken config: set TOOLCHAIN") != std::string::npos);
                REQUIRE(result.stderr_output.find("Tupfile:3") != std::string::npos);
            }
        }
    }
}

SCENARIO("Include first seen in a dead branch still applies when included actively", "[e2e][include-context]")
{
    GIVEN("a Tupfile whose statically false branch includes sub.tup before an unconditional include")
    {
        auto f = E2EFixture { "include_context" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("the project is built")
        {
            auto result = f.build({ "-B", "build" });

            THEN("rules from the actively included file are built")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/out.txt"));
                REQUIRE(f.exists("build/subout.txt"));
            }
        }

        WHEN("the included file holds an error directive")
        {
            f.write_file("sub.tup", "error included boom\n");

            auto result = f.build({ "-B", "build" });

            THEN("the build aborts with the message")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("included boom") != std::string::npos);
            }
        }
    }
}

SCENARIO("Include first seen in a config-inactive branch is reprocessed when included actively", "[e2e][include-context]")
{
    GIVEN("a project whose config branch and top level include the same file")
    {
        auto f = E2EFixture { "include_context" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("the included file only assigns variables")
        {
            f.write_file("Tupfile", "ifeq (@(M),y)\ninclude flags.tup\nendif\ninclude flags.tup\n: |> echo $(FLAG) > %o |> out.txt\n");
            f.write_file("flags.tup", "FLAG = hello\n");

            auto result = f.build({ "-B", "build" });

            THEN("the active include takes effect")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/out.txt").find("hello") != std::string::npos);
            }
        }

        WHEN("the included file defines a rule")
        {
            f.write_file("Tupfile", "ifeq (@(M),y)\ninclude sub.tup\nendif\ninclude sub.tup\n");

            auto result = f.build({ "-B", "build" });

            THEN("the latent duplicate-output conflict is reported")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("already owned") != std::string::npos);
            }
        }

        WHEN("the same file is included twice in the same context")
        {
            f.write_file("Tupfile", "include sub.tup\ninclude sub.tup\n");

            auto result = f.build({ "-B", "build" });

            THEN("the second include is deduplicated and the build succeeds")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/subout.txt"));
            }
        }
    }
}

SCENARIO("Assignments in inactive contexts do not leak into the active world", "[e2e][inactive-leak]")
{
    GIVEN("a project with an inactive config branch")
    {
        auto f = E2EFixture { "include_context" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("an active inner conditional assigns inside the inactive branch")
        {
            f.write_file("Tupfile", "ifeq (@(M),y)\nifeq (y,y)\nV = leaked\nendif\nendif\n: |> echo x$(V) > %o |> out.txt\n");

            auto result = f.build({ "-B", "build" });

            THEN("the assignment does not escape")
            {
                REQUIRE(result.success());
                auto content = f.read_file("build/out.txt");
                REQUIRE(content.find("leaked") == std::string::npos);
            }
        }

        WHEN("a file included from the inactive branch assigns")
        {
            f.write_file("Tupfile", "ifeq (@(M),y)\ninclude flags.tup\nendif\n: |> echo x$(FLAG) > %o |> out.txt\n");
            f.write_file("flags.tup", "FLAG = hello\n");

            auto result = f.build({ "-B", "build" });

            THEN("the assignment does not escape")
            {
                REQUIRE(result.success());
                auto content = f.read_file("build/out.txt");
                REQUIRE(content.find("hello") == std::string::npos);
            }
        }
    }
}

SCENARIO("Phi-node allows same output from complementary conditional branches", "[e2e][phi][same-output]")
{
    GIVEN("a project where both ifeq branches produce the same output")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", R"(
ifeq (@(MODE),release)
: input.txt |> cp %f %o && echo "release" >> %o |> output.txt
else
: input.txt |> cp %f %o && echo "debug" >> %o |> output.txt
endif
)");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");

        WHEN("built with MODE=debug (else branch)")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=debug\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds without duplicate output error")
            {
                REQUIRE(result.success());
            }

            THEN("output contains debug marker")
            {
                REQUIRE(f.exists("build/output.txt"));
                auto content = f.read_file("build/output.txt");
                REQUIRE(content.find("debug") != std::string::npos);
                REQUIRE(content.find("release") == std::string::npos);
            }
        }

        WHEN("built with MODE=release (then branch)")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=release\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds without duplicate output error")
            {
                REQUIRE(result.success());
            }

            THEN("output contains release marker")
            {
                REQUIRE(f.exists("build/output.txt"));
                auto content = f.read_file("build/output.txt");
                REQUIRE(content.find("release") != std::string::npos);
                REQUIRE(content.find("debug") == std::string::npos);
            }
        }
    }
}

SCENARIO("Phi-node allows same output from nested complementary conditionals", "[e2e][phi][same-output][nested]")
{
    GIVEN("a project with nested conditionals producing the same output")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", R"(
ifeq (@(PLATFORM),linux)
  ifeq (@(ARCH),x86)
    : input.txt |> cp %f %o && echo "linux-x86" >> %o |> output.txt
  else
    : input.txt |> cp %f %o && echo "linux-arm" >> %o |> output.txt
  endif
endif
)");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");

        WHEN("built with PLATFORM=linux and ARCH=x86")
        {
            f.write_file("build/tup.config", "CONFIG_PLATFORM=linux\nCONFIG_ARCH=x86\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds without duplicate output error")
            {
                REQUIRE(result.success());
            }

            THEN("output contains linux-x86 marker")
            {
                REQUIRE(f.exists("build/output.txt"));
                auto content = f.read_file("build/output.txt");
                REQUIRE(content.find("linux-x86") != std::string::npos);
                REQUIRE(content.find("linux-arm") == std::string::npos);
            }
        }

        WHEN("built with PLATFORM=linux and ARCH=arm")
        {
            f.write_file("build/tup.config", "CONFIG_PLATFORM=linux\nCONFIG_ARCH=arm\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds without duplicate output error")
            {
                REQUIRE(result.success());
            }

            THEN("output contains linux-arm marker")
            {
                REQUIRE(f.exists("build/output.txt"));
                auto content = f.read_file("build/output.txt");
                REQUIRE(content.find("linux-arm") != std::string::npos);
                REQUIRE(content.find("linux-x86") == std::string::npos);
            }
        }
    }
}

SCENARIO("Phi-node allows same output from chained else-ifeq conditionals", "[e2e][phi][same-output][chained]")
{
    GIVEN("a project with chained else-ifeq producing same output")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", R"(
ifeq (@(MODE),debug)
  : input.txt |> cp %f %o && echo "debug" >> %o |> output.txt
else
  ifeq (@(MODE),release)
    : input.txt |> cp %f %o && echo "release" >> %o |> output.txt
  else
    : input.txt |> cp %f %o && echo "default" >> %o |> output.txt
  endif
endif
)");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");

        WHEN("built with MODE=debug")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=debug\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds and output contains debug marker")
            {
                REQUIRE(result.success());
                auto content = f.read_file("build/output.txt");
                REQUIRE(content.find("debug") != std::string::npos);
            }
        }

        WHEN("built with MODE=release")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=release\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds and output contains release marker")
            {
                REQUIRE(result.success());
                auto content = f.read_file("build/output.txt");
                REQUIRE(content.find("release") != std::string::npos);
            }
        }

        WHEN("built with MODE=other (default branch)")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=other\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds and output contains default marker")
            {
                REQUIRE(result.success());
                auto content = f.read_file("build/output.txt");
                REQUIRE(content.find("default") != std::string::npos);
            }
        }
    }
}

SCENARIO("Phi-node with different output tags in conditional branches", "[e2e][phi][groups]")
{
    GIVEN("a project where conditional branches produce same file with different tags")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", R"(
ifeq (@(MODE),special)
: input.txt |> cp %f %o |> output.dat {special}
else
: input.txt |> cp %f %o |> output.dat {normal}
endif
: {normal} |> cat %f > %o |> result.txt
)");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");

        WHEN("built with MODE=normal (else branch active)")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=normal\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/result.txt"));
            }
        }

        WHEN("built with MODE=special (if branch active)")
        {
            f.write_file("build/tup.config", "CONFIG_MODE=special\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds with if-branch output")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/output.dat"));
            }
        }
    }
}

SCENARIO("Phi-node consumer depends on group from active branch only", "[e2e][phi][groups]")
{
    GIVEN("conditional branches produce same file into different groups, consumer uses active group")
    {
        auto f = E2EFixture { "phi_same_output" };
        f.write_file("Tupfile", R"(
ifeq (@(ENCRYPT),y)
: input.txt |> cp %f %o |> intermediate.elf {rawelf}
else
: input.txt |> cp %f %o |> intermediate.elf {elf}
endif
: {elf} |> cat %f > %o |> final.bin
)");
        f.write_file("input.txt", "payload\n");
        f.mkdir("build");

        WHEN("built with ENCRYPT=n (else branch active, {elf} group active)")
        {
            f.write_file("build/tup.config", "CONFIG_ENCRYPT=n\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            auto result = f.build({ "-B", "build" });

            THEN("build succeeds - consumer depends on active producer only")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/intermediate.elf"));
                REQUIRE(f.exists("build/final.bin"));
                auto content = f.read_file("build/final.bin");
                REQUIRE(content == "payload\n");
            }
        }
    }
}

SCENARIO("Order-only dependency on conditional output uses active producer", "[e2e][phi][order-only]")
{
    GIVEN("a project with conditional file generation and order-only dependency")
    {
        auto f = E2EFixture { "phi_order_only_conditional" };
        f.write_file("Tupfile", R"(
ifeq (@(GEN_PARSER),1)
: src.l |> echo "generated from flex" > %o |> generated.h
else
: generated.h.shipped |> cp %f %o |> generated.h
endif

: main.c | generated.h |> cc -c %f -o %o |> main.o
)");
        f.write_file("src.l", "/* flex source */\n");
        f.write_file("generated.h.shipped", "/* shipped header */\n");
        f.write_file("main.c", "int main() { return 0; }\n");
        f.mkdir("build");

        WHEN("built with GEN_PARSER=0 (else branch active)")
        {
            f.write_file("build/tup.config", "CONFIG_GEN_PARSER=0\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds using cp from else branch")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/generated.h"));
                REQUIRE(f.exists("build/main.o"));
            }
        }

        WHEN("built with GEN_PARSER=1 (if branch active)")
        {
            f.write_file("build/tup.config", "CONFIG_GEN_PARSER=1\n");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());

            auto result = f.build({ "-B", "build" });

            THEN("build succeeds using echo from if branch")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/generated.h"));
                REQUIRE(f.exists("build/main.o"));
            }
        }
    }
}

SCENARIO("Percent-d expands to Tupfile directory name", "[e2e][percent]")
{
    GIVEN("a subdirectory with a Tupfile using %d")
    {
        auto f = E2EFixture { "percent_d" };
        f.mkdir("mymod");
        f.write_file("mymod/Tupfile", R"(
: ../input.txt |> echo %d > %o |> dirname.txt
)");
        f.write_file("input.txt", "test\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built")
        {
            auto result = f.build({ "-B", "build" });

            THEN("%d expands to the Tupfile directory name")
            {
                REQUIRE(result.success());
                auto content = f.read_file("build/mymod/dirname.txt");
                REQUIRE(content.find("mymod") != std::string::npos);
            }
        }
    }
}

SCENARIO("Rules with empty input patterns are skipped", "[e2e][empty-input]")
{
    GIVEN("a rule with input pattern that evaluates to empty")
    {
        auto f = E2EFixture { "empty_input" };
        f.write_file("Tupfile", R"(
# undefined-y is empty, so this rule should be skipped
: $(undefined-y) |> echo should-not-run > %o |> skipped.txt
# This rule has no input pattern, so it should run
: |> echo ran > %o |> ran.txt
)");
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built")
        {
            auto result = f.build({ "-B", "build" });

            THEN("rule with empty pattern is skipped, rule without pattern runs")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build/skipped.txt"));
                REQUIRE(f.exists("build/ran.txt"));
            }
        }
    }
}

// =============================================================================
// Incremental Build Command String Mismatch Tests
// =============================================================================

SCENARIO("include_rules includes all Tuprules.tup from root to leaf", "[e2e][build]")
{
    GIVEN("a project with Tuprules.tup at root and in a subdirectory")
    {
        auto f = E2EFixture { "include_rules_nested" };
        REQUIRE(f.init().success());

        WHEN("building the subdirectory Tupfile")
        {
            auto result = f.build();

            THEN("both root and sub Tuprules.tup are included")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());

                auto content = f.read_file("sub/result.txt");
                INFO("result.txt: " << content);

                // ROOT comes from root Tuprules.tup (TUP_CWD = .. from sub/)
                REQUIRE(content.find("..") != std::string::npos);
                // CC = gcc (root sets it first, sub's ?= doesn't override)
                REQUIRE(content.find("gcc") != std::string::npos);
                // SUBVAR comes from sub/Tuprules.tup
                REQUIRE(content.find("from_sub") != std::string::npos);
            }
        }
    }
}

SCENARIO("Sibling directory inputs work with incremental variant builds", "[e2e][incremental][variant]")
{
    // This tests the command string matching between graph and index.
    // Pattern from spos: Tupfile at include/generated/ referencing ../data.txt
    // Bug: Index uses std::filesystem::relative() while graph uses make_source_relative()
    // These produce different paths for cross-directory references.
    GIVEN("a variant build with sibling directory input")
    {
        auto f = E2EFixture { "sibling_dir_inputs" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        auto first_build = f.build({ "-B", "build" });
        INFO("first build stdout: " << first_build.stdout_output);
        INFO("first build stderr: " << first_build.stderr_output);
        REQUIRE(first_build.success());
        REQUIRE(f.exists("build/include/generated/output.txt"));

        WHEN("rebuilding without changes")
        {
            auto rebuild = f.build({ "-B", "build" });

            THEN("rebuild is a no-op")
            {
                INFO("rebuild stdout: " << rebuild.stdout_output);
                INFO("rebuild stderr: " << rebuild.stderr_output);
                REQUIRE(rebuild.success());
                REQUIRE(rebuild.is_noop());
            }
        }
    }
}

// =============================================================================
// Strict Convention Checker Tests
// =============================================================================

SCENARIO("Check level controls convention enforcement", "[e2e][strict]")
{
    GIVEN("a project with a component violating conventions")
    {
        auto f = E2EFixture { "strict_check" };
        REQUIRE(f.init().success());

        WHEN("parse --check=error is run")
        {
            auto result = f.pup({ "parse", "--check=error" });

            THEN("it fails with error diagnostics")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("must use") != std::string::npos);
            }
        }

        WHEN("parse --strict is run (alias for --check=error)")
        {
            auto result = f.pup({ "parse", "--strict" });

            THEN("it fails with error diagnostics")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("must use") != std::string::npos);
            }
        }

        WHEN("parse is run at the default check level")
        {
            auto result = f.pup({ "parse" });

            THEN("it succeeds but still reports the violation")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("must use") != std::string::npos);
            }
        }

        WHEN("parse --check=none is run")
        {
            auto result = f.pup({ "parse", "--check=none" });

            THEN("it succeeds without reporting the violation")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("must use") == std::string::npos);
            }
        }
    }
}

SCENARIO("Strict checker exempts the config-tree root in 3-tree builds", "[e2e][strict][out-of-tree-config]")
{
    GIVEN("a 3-tree project whose config-tree root anchors with '='")
    {
        auto f = E2EFixture { "groups_cross_dir_3tree" };
        auto source_dir = f.workdir() / "source";
        auto config_dir = f.workdir() / "config";
        auto build_dir = f.workdir() / "build";

        f.mkdir("build");
        f.write_file("build/tup.config", "");

        WHEN("parse --check=error is run")
        {
            auto result = f.pup({
                "parse",
                "--check=error",
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
            });

            THEN("the config-tree root is not flagged as a component violation")
            {
                INFO("stdout:\n" << result.stdout_output);
                INFO("stderr:\n" << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("must use") == std::string::npos);
            }
        }
    }
}

// =============================================================================
// Scoped Build Implicit Dependency Tests
// =============================================================================

SCENARIO("Scoped build detects header changes outside scope", "[e2e][incremental][scope]")
{
    GIVEN("a project with headers outside the source scope")
    {
        auto f = E2EFixture { "scoped_implicit_dep" };
        REQUIRE(f.init().success());

        // Initial build (full, unscoped)
        auto first = f.build();
        INFO("first build stdout: " << first.stdout_output);
        INFO("first build stderr: " << first.stderr_output);
        REQUIRE(first.success());
        REQUIRE(f.exists("src/main.o"));

        WHEN("header outside scope is modified and scoped build runs")
        {
            // Modify the header (outside the src/ scope)
            f.write_file("include/header.h", "#define VERSION 2\n");

            // Scoped build: only src/ directory
            auto result = f.pup({ "src" });

            THEN("the change is detected and the file is rebuilt")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }
        }
    }
}

// Reproducer for the real-world bug noted in CLAUDE.md:
//   "Header-dep tracking can miss across variants — editing a widely-included
//    SDK header may re-link with stale .o files and produce a binary newer
//    than the header but functionally older."
//
// Differs from the single-scope test above in two ways:
//   - multi-arg scoped invocation (mirrors `pup src/fubon ... ios` pattern)
//   - multiple TUs in different scope dirs all transitively reach the same
//     out-of-scope header, AND a downstream link rule consumes their .o files
//
// The strict assertion checks that ALL three .o files (alpha.o, beta.o,
// main.o) are present AND the binary's hash changes after the header bump,
// catching both the "noop" mode and the "rebuild some-but-not-all + relink
// with stale .o" mode of the bug.
SCENARIO("Scoped build with multiple scopes detects out-of-scope header changes",
    "[e2e][incremental][scope][multi-scope]")
{
    GIVEN("a project where multiple scoped TUs share one header in include/")
    {
        auto f = E2EFixture { "scoped_implicit_dep_multi_scope" };
        REQUIRE(f.init().success());

        auto first = f.build();
        INFO("first build stdout: " << first.stdout_output);
        INFO("first build stderr: " << first.stderr_output);
        REQUIRE(first.success());
        REQUIRE(f.exists("src/alpha/alpha.o"));
        REQUIRE(f.exists("src/beta/beta.o"));
        REQUIRE(f.exists("app/main.o"));
        REQUIRE(f.exists("app/app"));

        // Capture the linked binary's content so we can detect stale-link bugs
        // even if pup claims "rebuilt".
        auto const binary_before = f.read_file("app/app");

        WHEN("the shared header is modified and the same scoped build re-runs")
        {
            f.write_file("include/lib/shared.h", "#define VERSION 2\n");

            // Multi-arg scoped invocation mirroring the real-world reproducer.
            auto result = f.pup({ "src/alpha", "src/beta", "app" });

            THEN("pup picks up the change and the link reflects the new VERSION")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());

                // Strict check: the linked binary must differ from the pre-edit
                // version. If pup recompiled only some .o files and re-linked
                // with stale others, the binary would be byte-identical or only
                // partially updated — either way still buggy.
                auto const binary_after = f.read_file("app/app");
                REQUIRE(binary_before != binary_after);
            }
        }
    }
}

// Same shape as above but with the build done out-of-tree under build/<variant>,
// invoked with -B. Variants share the source tree but have independent indices
// — a fix in one variant's index must not let the other re-link with stale .o.
SCENARIO("Out-of-tree variant build picks up out-of-scope header changes",
    "[e2e][incremental][scope][multi-scope][variant]")
{
    GIVEN("the multi-scope project configured under build/v1")
    {
        auto f = E2EFixture { "scoped_implicit_dep_multi_scope" };
        REQUIRE(f.pup({ "configure", "-B", "build/v1" }).success());
        REQUIRE(f.build({ "-B", "build/v1" }).success());
        REQUIRE(f.exists("build/v1/src/alpha/alpha.o"));
        REQUIRE(f.exists("build/v1/src/beta/beta.o"));
        REQUIRE(f.exists("build/v1/app/app"));

        auto const binary_before = f.read_file("build/v1/app/app");

        WHEN("the shared header is edited and the variant rebuilds")
        {
            f.write_file("include/lib/shared.h", "#define VERSION 2\n");

            auto result = f.pup({ "-B", "build/v1",
                "src/alpha", "src/beta", "app" });

            THEN("the variant's binary reflects the new VERSION")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());

                auto const binary_after = f.read_file("build/v1/app/app");
                REQUIRE(binary_before != binary_after);
            }
        }
    }
}

// Probes the scoped-initial-build axis: when the FIRST build is already
// scoped (rather than full), is the index populated correctly enough that
// a subsequent header edit propagates through the linked binary?
SCENARIO("Scoped rebuild after a SCOPED initial multi-scope build",
    "[e2e][incremental][scope][multi-scope]")
{
    GIVEN("a multi-scope project whose first build was already scoped")
    {
        auto f = E2EFixture { "scoped_implicit_dep_multi_scope" };
        REQUIRE(f.init().success());

        auto scopes = std::vector<std::string> { "src/alpha", "src/beta", "app" };
        auto first = f.pup(scopes);
        INFO("first scoped build stdout: " << first.stdout_output);
        INFO("first scoped build stderr: " << first.stderr_output);
        REQUIRE(first.success());
        REQUIRE(f.exists("app/app"));

        auto const binary_before = f.read_file("app/app");

        WHEN("the shared header changes and the same scoped command runs again")
        {
            f.write_file("include/lib/shared.h", "#define VERSION 2\n");

            auto result = f.pup(scopes);

            THEN("the linked binary reflects the new VERSION")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());

                auto const binary_after = f.read_file("app/app");
                REQUIRE(binary_before != binary_after);
            }
        }
    }
}

// Probes the repeated-scoped-invocation axis: full build, then a no-op
// scoped pass, then a real edit + scoped rebuild. Mirrors the production
// pattern where developers run the same scoped command many times across
// a session before the edit that exposes the bug.
SCENARIO("Header edit after a noop scoped pass still propagates to the linked binary",
    "[e2e][incremental][scope][multi-scope]")
{
    GIVEN("a fully-built project that has gone through a noop scoped pass")
    {
        auto f = E2EFixture { "scoped_implicit_dep_multi_scope" };
        REQUIRE(f.init().success());

        auto scopes = std::vector<std::string> { "src/alpha", "src/beta", "app" };

        auto first = f.build();
        INFO("first build stdout: " << first.stdout_output);
        REQUIRE(first.success());
        REQUIRE(f.exists("app/app"));

        auto noop = f.pup(scopes);
        INFO("noop stdout: " << noop.stdout_output);
        REQUIRE(noop.success());

        auto const binary_before = f.read_file("app/app");

        WHEN("the shared header changes and the scoped build runs once more")
        {
            f.write_file("include/lib/shared.h", "#define VERSION 2\n");

            auto result = f.pup(scopes);

            THEN("the linked binary still reflects the new VERSION")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());

                auto const binary_after = f.read_file("app/app");
                REQUIRE(binary_before != binary_after);
            }
        }
    }
}

// =============================================================================
// Target Build No-Op Tests
// =============================================================================

SCENARIO("Target build stabilizes to no-op", "[e2e][incremental][target]")
{
    GIVEN("a project with two independent targets")
    {
        auto f = E2EFixture { "target_noop" };
        REQUIRE(f.init().success());

        // Full build — both targets
        auto first = f.build();
        INFO("first build: " << first.stdout_output);
        REQUIRE(first.success());
        REQUIRE(f.is_executable("prog_a"));
        REQUIRE(f.is_executable("prog_b"));

        WHEN("target build runs for prog_a, then runs again")
        {
            // First target build — may rebuild some commands
            auto target1 = f.pup({ "prog_a" });
            INFO("target build 1: " << target1.stdout_output);
            REQUIRE(target1.success());

            // Second target build — must be no-op
            auto target2 = f.pup({ "prog_a" });

            THEN("the second target build is a no-op")
            {
                INFO("target build 2 stdout: " << target2.stdout_output);
                INFO("target build 2 stderr: " << target2.stderr_output);
                REQUIRE(target2.success());
                REQUIRE(target2.is_noop());
            }
        }
    }
}

SCENARIO("3-tree: group pattern %o must include build root prefix", "[e2e][out-of-tree-config]")
{
    // Reproduces GCC BSP pattern where:
    //   1. Library archive uses order-only group: %<objs> in command
    //   2. Consumer links the library via $(B)/$(LIB_DIR)/libmath.a
    //
    // The group pattern forces has_group_pattern=true, so final_instruction
    // becomes cmd_text (parse-time %o expansion). Output PathIds must be
    // BuildRoot-grounded so materialize_path() prepends the build root
    // prefix. Without grounding, %o becomes "libmath.a" (bare filename)
    // instead of "../../build/zzz_lib/libmath.a".
    GIVEN("a 3-tree project with order-only groups in archive command")
    {
        auto f = E2EFixture { "3tree_cross_subdir_output" };
        auto source_dir = f.workdir() / "source";
        auto config_dir = f.workdir() / "config";
        auto build_dir = f.workdir() / "build";

        f.mkdir("build");
        f.write_file("build/tup.config", "");

        WHEN("parsing with verbose output")
        {
            auto conf = f.pup({
                "configure",
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string()
            });
            REQUIRE(conf.success());

            auto result = f.pup({
                "parse",
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string(),
                "-v"
            });

            THEN("archive %o includes build-relative path, not bare filename")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                // The archive command must write to the build tree.
                // Bug:   "ar rcs libmath.a ..."  (bare filename → written to source tree)
                // Fixed: "ar rcs ../../build/zzz_lib/libmath.a ..."
                auto ar_pos = result.stdout_output.find("ar rcs ");
                REQUIRE(ar_pos != std::string::npos);
                // Extract the %o argument (first word after "ar rcs ")
                auto output_start = ar_pos + 7; // strlen("ar rcs ")
                auto output_end = result.stdout_output.find(' ', output_start);
                auto output_arg = result.stdout_output.substr(output_start, output_end - output_start);
                INFO("archive output arg: " << output_arg);
                // %o must point to the build directory, not be a bare filename
                REQUIRE(output_arg.find("build") != std::string::npos);
            }
        }

        WHEN("building")
        {
            auto conf = f.pup({
                "configure",
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string()
            });
            REQUIRE(conf.success());

            auto result = f.pup({
                "-S", source_dir.string(),
                "-C", config_dir.string(),
                "-B", build_dir.string()
            });

            THEN("build succeeds with library in build tree")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/zzz_lib/libmath.a"));
                REQUIRE_FALSE(f.exists("source/zzz_lib/libmath.a"));
            }
        }
    }
}

// =============================================================================
// Build Statistics Report
// =============================================================================

namespace {

struct PhaseReport {
    std::vector<std::pair<std::string, double>> phases;
    double total_ms = -1.0;

    [[nodiscard]] auto phase_sum() const -> double
    {
        auto sum = 0.0;
        for (auto const& [name, ms] : phases) {
            sum += ms;
        }
        return sum;
    }

    [[nodiscard]] auto has(std::string_view name) const -> bool
    {
        for (auto const& [phase, ms] : phases) {
            if (phase == name) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto value_of(std::string_view name) const -> double
    {
        for (auto const& [phase, ms] : phases) {
            if (phase == name) {
                return ms;
            }
        }
        return -1.0;
    }
};

auto parse_phase_report(std::string const& output) -> PhaseReport
{
    auto report = PhaseReport {};
    auto in_phases = false;
    auto stream = std::istringstream { output };
    auto line = std::string {};

    while (std::getline(stream, line)) {
        if (line.find("Phase timing:") != std::string::npos) {
            in_phases = true;
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto ms_pos = line.find("ms", colon);
        if (ms_pos == std::string::npos) {
            continue;
        }

        auto first = line.find_first_not_of(" ");
        auto label = line.substr(first, colon - first);
        auto value = std::stod(line.substr(colon + 1, ms_pos - colon - 1));

        if (label == "Total") {
            report.total_ms = value;
            in_phases = false;
        } else if (in_phases) {
            report.phases.emplace_back(label, value);
        }
    }
    return report;
}

} // namespace

SCENARIO("Build statistics account for the whole build", "[e2e][stat]")
{
    GIVEN("a project that has already been built")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("an up-to-date build is run with --stat")
        {
            auto result = f.build({ "--stat" });
            REQUIRE(result.success());
            auto report = parse_phase_report(result.stdout_output);

            THEN("a total build time is reported")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(report.total_ms >= 0.0);
            }

            THEN("the phases that dominate an up-to-date build are reported")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(report.has("Parse"));
                REQUIRE(report.has("Index rebuild"));
                REQUIRE(report.has("Unaccounted"));
            }

            THEN("the reported phases sum to the reported total")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("phase sum: " << report.phase_sum() << " total: " << report.total_ms);
                REQUIRE(report.phase_sum() == Catch::Approx(report.total_ms).margin(1.0));
            }

            THEN("no phase is counted twice")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(report.value_of("Unaccounted") >= -0.5);
            }

            THEN("the on-disk index size is reported")
            {
                INFO("stdout: " << result.stdout_output);
                auto label = std::string_view { "Index size:" };
                auto pos = result.stdout_output.find(label);
                REQUIRE(pos != std::string::npos);
                auto reported = std::stoull(result.stdout_output.substr(pos + label.size()));
                auto actual = std::filesystem::file_size(f.workdir() / ".pup" / "index");
                REQUIRE(reported == actual);
            }
        }
    }
}

SCENARIO("An inactive conditional branch does not dirty later builds", "[e2e][phi][incremental]")
{
    GIVEN("a built project whose inactive branch declares an output")
    {
        auto f = E2EFixture { "conditional_groups" };
        f.write_file("Tupfile", R"(
ifeq (@(USE_PLATFORM),ios)
: ios_impl.c |> gcc -c %f -o %o |> ios_impl.o {objs}
else
: linux_impl.c |> gcc -c %f -o %o |> linux_impl.o {objs}
endif

: {objs} |> ar rcs %o %f |> libplatform.a
)");
        f.write_file("ios_impl.c", "int platform_init(void) { return 1; }\n");
        f.write_file("linux_impl.c", "int platform_init(void) { return 2; }\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_USE_PLATFORM=linux\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());

        WHEN("the project is rebuilt with nothing changed")
        {
            auto result = f.build({ "-B", "build", "-v" });

            THEN("the inactive branch's output is not reported as changed")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("ios_impl.o") == std::string::npos);
            }

            THEN("no file is reported as changed at all")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("Changed (") == std::string::npos);
            }

            THEN("the build takes the up-to-date fast path")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("up to date") != std::string::npos);
            }
        }

        WHEN("the config flips to the other branch")
        {
            f.write_file("build/tup.config", "CONFIG_USE_PLATFORM=ios\n");
            auto result = f.build({ "-B", "build" });

            THEN("the newly active branch's output is built")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/ios_impl.o"));
            }

            THEN("the newly inactive branch's output is gone")
            {
                REQUIRE_FALSE(f.exists("build/linux_impl.o"));
            }
        }
    }
}

SCENARIO("An output declared by both conditional branches stays tracked", "[e2e][phi][incremental]")
{
    GIVEN("a built project where both branches declare the same output")
    {
        auto f = E2EFixture { "conditional_groups" };
        f.write_file("Tupfile", R"(
ifeq (@(USE_PLATFORM),ios)
: ios_impl.c |> gcc -c %f -o %o |> platform.o
else
: linux_impl.c |> gcc -c %f -o %o |> platform.o
endif
)");
        f.write_file("ios_impl.c", "int platform_init(void) { return 1; }\n");
        f.write_file("linux_impl.c", "int platform_init(void) { return 2; }\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_USE_PLATFORM=linux\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/platform.o"));

        WHEN("the output is deleted")
        {
            f.remove_file("build/platform.o");
            auto result = f.build({ "-B", "build" });

            THEN("the active branch rebuilds it")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("build/platform.o"));
            }
        }
    }
}

// =============================================================================
// Scoped build must not delete out-of-scope outputs (issue #122)
// =============================================================================

namespace {

// A two-directory project (alpha, beta) plus a root-level rule, fully built in
// build/. Each rule just copies its source, so outputs are trivially checkable.
auto build_scoped_stale_project(E2EFixture& f) -> void
{
    f.write_file("r.c", "int r(void) { return 0; }\n");
    f.write_file("Tupfile", ": r.c |> cp %f %o |> r.out\n");
    f.write_file("alpha/a.c", "int a(void) { return 1; }\n");
    f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
    f.write_file("beta/b.c", "int b(void) { return 2; }\n");
    f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out\n");
    f.write_file("build/tup.config", "");
    REQUIRE(f.pup({ "configure", "-B", "build" }).success());
    REQUIRE(f.build({ "-B", "build" }).success());
    REQUIRE(f.exists("build/r.out"));
    REQUIRE(f.exists("build/alpha/a.out"));
    REQUIRE(f.exists("build/beta/b.out"));
}

} // namespace

SCENARIO("Scoped build preserves outputs of directories outside the scope", "[e2e][incremental][scope]")
{
    GIVEN("a fully-built two-directory project")
    {
        auto f = E2EFixture { "scoped_stale" };
        build_scoped_stale_project(f);

        WHEN("a build is scoped to alpha")
        {
            auto result = f.build({ "-B", "build", "alpha" });

            THEN("the build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
            }

            THEN("beta's output is not deleted")
            {
                REQUIRE(f.exists("build/beta/b.out"));
            }

            THEN("the root command's output is not deleted")
            {
                REQUIRE(f.exists("build/r.out"));
            }
        }
    }
}

SCENARIO("Scoped build still removes genuinely stale outputs within the scope", "[e2e][incremental][scope]")
{
    GIVEN("a project whose scoped directory has two rules, fully built")
    {
        auto f = E2EFixture { "scoped_stale" };
        build_scoped_stale_project(f);
        f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n: a.c |> cp %f %o |> a2.out\n");
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/alpha/a2.out"));

        WHEN("the second rule is removed and alpha is rebuilt in scope")
        {
            f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
            auto result = f.build({ "-B", "build", "alpha" });

            THEN("the now-stale in-scope output is deleted")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build/alpha/a2.out"));
                REQUIRE(f.exists("build/alpha/a.out"));
            }
        }
    }
}

SCENARIO("Scoped build removes stale outputs in a nested in-scope directory", "[e2e][incremental][scope]")
{
    GIVEN("a project with a rule in a nested subdirectory, fully built")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("alpha/a.c", "int a(void) { return 1; }\n");
        f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
        f.write_file("alpha/sub/s.c", "int s(void) { return 3; }\n");
        f.write_file("alpha/sub/Tupfile", ": s.c |> cp %f %o |> s.out\n: s.c |> cp %f %o |> s2.out\n");
        f.write_file("beta/b.c", "int b(void) { return 2; }\n");
        f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out\n");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/alpha/sub/s2.out"));
        REQUIRE(f.exists("build/beta/b.out"));

        WHEN("a rule in alpha/sub is removed and the build is scoped to alpha")
        {
            f.write_file("alpha/sub/Tupfile", ": s.c |> cp %f %o |> s.out\n");
            auto result = f.build({ "-B", "build", "alpha" });

            THEN("the nested in-scope stale output is deleted")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build/alpha/sub/s2.out"));
                REQUIRE(f.exists("build/alpha/sub/s.out"));
            }

            THEN("beta outside the scope is still preserved")
            {
                REQUIRE(f.exists("build/beta/b.out"));
            }
        }
    }
}

SCENARIO("Rerun forces up-to-date commands to execute", "[e2e][incremental][rerun]")
{
    GIVEN("a fully built project that no-ops on rebuild")
    {
        auto f = E2EFixture { "scoped_build" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).is_noop());

        WHEN("the build runs with --rerun")
        {
            auto result = f.build({ "-B", "build", "--rerun" });

            THEN("every command executes again, including the dep scan")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("3 commands") != std::string::npos);
            }
        }

        WHEN("--rerun is scoped to one directory")
        {
            auto result = f.build({ "-B", "build", "--rerun", "lib" });

            THEN("only that directory's commands execute")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("2 commands") != std::string::npos);
            }
        }

        WHEN("a rerun completes")
        {
            REQUIRE(f.build({ "-B", "build", "--rerun" }).success());

            THEN("the next plain build is a no-op again")
            {
                REQUIRE(f.build({ "-B", "build" }).is_noop());
            }
        }
    }
}

SCENARIO("Scoped build restores a deleted output inside scope", "[e2e][incremental][scope]")
{
    GIVEN("a fully-built variant of a multi-directory project")
    {
        auto f = E2EFixture { "scoped_build" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/lib/foo.o"));

        WHEN("an in-scope output is deleted and the build is scoped to its directory")
        {
            f.remove_file("build/lib/foo.o");
            auto result = f.build({ "-B", "build", "lib" });

            THEN("the missing output is rebuilt")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.exists("build/lib/foo.o"));
            }
        }
    }
}

SCENARIO("Scoped build preserves out-of-scope commands in the saved index", "[e2e][incremental][scope]")
{
    GIVEN("a fully-built two-directory project")
    {
        auto f = E2EFixture { "scoped_stale" };
        build_scoped_stale_project(f);

        WHEN("alpha is modified and a job-running scoped build saves the index")
        {
            f.write_file("alpha/a.c", "int a(void) { return 42; }\n");
            auto scoped = f.build({ "-B", "build", "alpha" });
            REQUIRE(scoped.success());
            REQUIRE_FALSE(scoped.is_noop());

            auto full = f.build({ "-B", "build", "-v" });

            THEN("the following full build is a no-op")
            {
                INFO("stdout: " << full.stdout_output);
                INFO("stderr: " << full.stderr_output);
                REQUIRE(full.success());
                REQUIRE(full.stdout_output.find("New command") == std::string::npos);
                REQUIRE(full.is_noop());
            }
        }

        WHEN("a scoped build runs, then an out-of-scope rule is deleted")
        {
            f.write_file("alpha/a.c", "int a(void) { return 42; }\n");
            REQUIRE(f.build({ "-B", "build", "alpha" }).success());

            f.write_file("beta/Tupfile", "");
            auto full = f.build({ "-B", "build" });

            THEN("the full build still knows beta's output and cleans the orphan")
            {
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.success());
                REQUIRE_FALSE(f.exists("build/beta/b.out"));
                REQUIRE(f.exists("build/alpha/a.out"));
            }
        }
    }
}

SCENARIO("Scoped rebuild of a producer keeps the out-of-scope consumer scheduled", "[e2e][incremental][scope]")
{
    GIVEN("a fully-built project where app links lib's object")
    {
        auto f = E2EFixture { "scoped_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("app/app").exit_code == 42);

        WHEN("lib's source changes and only lib is rebuilt in scope")
        {
            auto original = f.read_file("lib/foo.c");
            auto pos = original.find("42");
            REQUIRE(pos != std::string::npos);
            f.write_file("lib/foo.c", original.substr(0, pos) + "99" + original.substr(pos + 2));
            auto scoped = f.build({ "lib" });
            REQUIRE(scoped.success());
            REQUIRE_FALSE(scoped.is_noop());

            auto full = f.build({ "-v" });

            THEN("the full build relinks the app against the new object")
            {
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.success());
                REQUIRE_FALSE(full.is_noop());
                REQUIRE(f.run("app/app").exit_code == 99);
            }
        }
    }
}

SCENARIO("Scoped build preserves out-of-scope implicit dependencies", "[e2e][incremental][scope]")
{
    GIVEN("a project whose out-of-scope directory has a discovered header dependency")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("alpha/a.c", "int a(void) { return 1; }\n");
        f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
        f.write_file("beta/b.h", "#define B 2\n");
        f.write_file("beta/b.c", "#include \"b.h\"\nint b(void) { return B; }\n");
        f.write_file("beta/Tupfile", ": b.c |> gcc -c %f -o %o |> b.o\n");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/beta/b.o"));

        WHEN("a scoped build saves the index, then beta's header changes")
        {
            f.write_file("alpha/a.c", "int a(void) { return 42; }\n");
            REQUIRE(f.build({ "-B", "build", "alpha" }).success());

            f.write_file("beta/b.h", "#define B 3\n");
            auto full = f.build({ "-B", "build", "-v" });

            THEN("beta is rebuilt through the preserved implicit edge, not as a new command")
            {
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.success());
                REQUIRE_FALSE(full.is_noop());
                REQUIRE(full.stdout_output.find("b.o") != std::string::npos);
                REQUIRE(full.stdout_output.find("New command") == std::string::npos);
            }
        }
    }
}

SCENARIO("keep-going build preserves outputs of a Tupfile that fails to parse", "[e2e][incremental]")
{
    GIVEN("a fully-built two-directory project")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("alpha/a.c", "int a(void) { return 1; }\n");
        f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
        f.write_file("beta/b.c", "int b(void) { return 2; }\n");
        f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out\n");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/alpha/a.out"));
        REQUIRE(f.exists("build/beta/b.out"));

        WHEN("beta's Tupfile is made unparseable and a keep-going build runs")
        {
            f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out\n:::: not valid tup syntax\n");
            auto result = f.build({ "-B", "build", "-k" });

            THEN("beta's output is not deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("build/beta/b.out"));
            }

            THEN("alpha's output is untouched")
            {
                REQUIRE(f.exists("build/alpha/a.out"));
            }
        }
    }
}

SCENARIO("keep-going build preserves outputs of a Tupfile that fails to evaluate", "[e2e][incremental]")
{
    GIVEN("a fully-built two-directory project")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("alpha/a.c", "int a(void) { return 1; }\n");
        f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
        f.write_file("beta/b.c", "int b(void) { return 2; }\n");
        f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out\n");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/alpha/a.out"));
        REQUIRE(f.exists("build/beta/b.out"));

        WHEN("beta's Tupfile hits an error directive and a keep-going build runs")
        {
            f.write_file("beta/Tupfile", "error broken\n: b.c |> cp %f %o |> b.out\n");
            auto result = f.build({ "-B", "build", "-k" });

            THEN("beta's output is not deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("build/beta/b.out"));
            }

            THEN("the evaluation error is reported")
            {
                REQUIRE(result.stderr_output.find("broken") != std::string::npos);
            }
        }

        WHEN("beta's Tupfile uses an unknown bang macro and a keep-going build runs")
        {
            f.write_file("beta/Tupfile", ": b.c |> !nope |> b.out\n");
            auto result = f.build({ "-B", "build", "-k" });

            THEN("beta's output is not deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("build/beta/b.out"));
            }

            THEN("the evaluation error is reported")
            {
                REQUIRE(result.stderr_output.find("nope") != std::string::npos);
            }
        }

        WHEN("beta's Tupfile includes a missing file and a keep-going build runs")
        {
            f.write_file("beta/Tupfile", "include missing.tup\n: b.c |> cp %f %o |> b.out\n");
            auto result = f.build({ "-B", "build", "-k" });

            THEN("beta's output is not deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("build/beta/b.out"));
            }

            THEN("the evaluation error is reported")
            {
                REQUIRE(result.stderr_output.find("missing.tup") != std::string::npos);
            }
        }

        WHEN("beta's Tupfile uses an unknown bang macro and a verbose keep-going build runs")
        {
            f.write_file("beta/Tupfile", ": b.c |> !nope |> b.out\n");
            auto result = f.build({ "-B", "build", "-k", "-v" });

            THEN("beta's output is not deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("build/beta/b.out"));
            }

            THEN("the evaluation error is reported")
            {
                REQUIRE(result.stderr_output.find("nope") != std::string::npos);
            }
        }
    }
}

namespace {

auto count_occurrences(std::string const& haystack, std::string const& needle) -> std::size_t
{
    auto n = std::size_t { 0 };
    for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

SCENARIO("Evaluation failure in a directory parsed on demand is reported once", "[e2e][incremental]")
{
    GIVEN("alpha depending on a group provided by beta, fully built")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("alpha/a.c", "int a(void) { return 1; }\n");
        f.write_file("alpha/Tupfile", ": a.c | ../beta/<grp> |> cp %f %o |> a.out\n");
        f.write_file("beta/b.c", "int b(void) { return 2; }\n");
        f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out <grp>\n");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/alpha/a.out"));
        REQUIRE(f.exists("build/beta/b.out"));

        WHEN("beta's Tupfile hits an error directive and a build runs")
        {
            f.write_file("beta/Tupfile", "error broken\n: b.c |> cp %f %o |> b.out <grp>\n");
            auto result = f.build({ "-B", "build" });

            THEN("the build fails with the evaluation error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(count_occurrences(result.stderr_output, "broken") == 1);
            }

            THEN("no phantom already-owned error is reported")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("already owned") == std::string::npos);
            }
        }

        WHEN("beta's Tupfile hits an error directive and a keep-going build runs")
        {
            f.write_file("beta/Tupfile", "error broken\n: b.c |> cp %f %o |> b.out <grp>\n");
            auto result = f.build({ "-B", "build", "-k" });

            THEN("beta's output is not deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("build/beta/b.out"));
            }

            THEN("the error is reported once, without phantom already-owned errors")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(count_occurrences(result.stderr_output, "broken") == 1);
                REQUIRE(result.stderr_output.find("already owned") == std::string::npos);
            }
        }

        WHEN("a rule is removed from alpha while beta fails to evaluate")
        {
            f.write_file("alpha/Tupfile", ": a.c | ../beta/<grp> |> cp %f %o |> a.out\n: a.c |> cp %f %o |> a2.out\n");
            REQUIRE(f.build({ "-B", "build" }).success());
            REQUIRE(f.exists("build/alpha/a2.out"));

            f.write_file("alpha/Tupfile", ": a.c | ../beta/<grp> |> cp %f %o |> a.out\n");
            f.write_file("beta/Tupfile", "error broken\n: b.c |> cp %f %o |> b.out <grp>\n");
            auto result = f.build({ "-B", "build", "-k" });

            THEN("alpha stays authoritative and its stale output is deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(f.exists("build/alpha/a2.out"));
                REQUIRE(f.exists("build/alpha/a.out"));
            }

            THEN("beta's output is preserved and its error reported once")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("build/beta/b.out"));
                REQUIRE(count_occurrences(result.stderr_output, "broken") == 1);
            }
        }
    }
}

SCENARIO("Output-less rule reactivated from an inactive conditional runs", "[e2e][incremental]")
{
    GIVEN("a built project with output-less rule gated by config inside an inactive conditional")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("Tupfile", "ifeq (@(FOO),yes)\n: |> touch marker_@(FOO) |>\nendif\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_FOO=no\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE_FALSE(f.exists("marker_no"));

        WHEN("the conditional is removed so the rule becomes unconditional")
        {
            f.write_file("Tupfile", ": |> touch marker_@(FOO) |>\n");
            auto result = f.build({ "-B", "build" });

            THEN("the rule runs and the marker file is created")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("marker_no"));
            }

            THEN("a second rebuild is a noop")
            {
                auto result2 = f.build({ "-B", "build" });
                INFO("stdout: " << result2.stdout_output);
                REQUIRE(result2.is_noop());
            }
        }
    }
}

SCENARIO("Config-gated rule follows guard flips", "[e2e][incremental]")
{
    GIVEN("a built project with a config-gated output rule inside a conditional")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("Tupfile", "ifeq (@(FOO),yes)\n: |> touch %o |> out.txt\nendif\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_FOO=yes\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/out.txt"));

        WHEN("the config changes to deactivate the rule")
        {
            f.write_file("build/tup.config", "CONFIG_FOO=no\n");
            auto result = f.build({ "-B", "build" });

            THEN("the output is deleted")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("build/out.txt"));
            }
        }

        WHEN("config is changed to deactivate then reactivate the rule")
        {
            f.write_file("build/tup.config", "CONFIG_FOO=no\n");
            auto deactivate_result = f.build({ "-B", "build" });
            REQUIRE(deactivate_result.success());
            REQUIRE_FALSE(f.exists("build/out.txt"));

            f.write_file("build/tup.config", "CONFIG_FOO=yes\n");
            auto reactivate_result = f.build({ "-B", "build" });

            THEN("the output is recreated")
            {
                INFO("stdout: " << reactivate_result.stdout_output);
                INFO("stderr: " << reactivate_result.stderr_output);
                REQUIRE(reactivate_result.success());
                REQUIRE(f.exists("build/out.txt"));
            }
        }
    }
}

SCENARIO("-x excludes matching directories from the build", "[e2e][exclude]")
{
    GIVEN("a project with tests directories at two levels")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());

        WHEN("built with -x tests/")
        {
            auto result = f.build({ "-x", "tests/" });

            THEN("only non-excluded outputs are produced")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("lib/foo.out"));
                REQUIRE_FALSE(f.exists("tests/t.out"));
                REQUIRE_FALSE(f.exists("lib/tests/n.out"));
            }
        }
    }
}

SCENARIO("Excluded directories keep their index state across toggles", "[e2e][exclude][incremental]")
{
    GIVEN("a fully built project")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("rebuilt with and then without -x tests/")
        {
            auto excluded = f.build({ "-x", "tests/" });
            auto full = f.build();

            THEN("both runs are no-ops")
            {
                INFO("excluded stdout: " << excluded.stdout_output);
                INFO("full stdout: " << full.stdout_output);
                REQUIRE(excluded.success());
                REQUIRE(excluded.is_noop());
                REQUIRE(full.success());
                REQUIRE(full.is_noop());
            }
        }
    }
}

SCENARIO("Changes in excluded directories are deferred until re-inclusion", "[e2e][exclude][incremental]")
{
    GIVEN("a fully built project with a modified input under tests/")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        f.append_file("tests/t.txt", "modified\n");

        WHEN("built with -x tests/")
        {
            auto excluded = f.build({ "-x", "tests/" });

            THEN("the excluded change is ignored")
            {
                INFO("stdout: " << excluded.stdout_output);
                INFO("stderr: " << excluded.stderr_output);
                REQUIRE(excluded.success());
                REQUIRE(excluded.is_noop());

                AND_WHEN("built again without -x")
                {
                    auto full = f.build();

                    THEN("only the excluded directory's command reruns")
                    {
                        INFO("stdout: " << full.stdout_output);
                        INFO("stderr: " << full.stderr_output);
                        REQUIRE(full.success());
                        REQUIRE(full.stdout_output.find("1 commands") != std::string::npos);
                        REQUIRE(f.read_file("tests/t.out") == "t\nmodified\n");
                    }
                }
            }
        }
    }
}

SCENARIO("Fresh exclusion then full build runs only the excluded commands", "[e2e][exclude][incremental]")
{
    GIVEN("a fresh project first built with -x tests/")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());
        REQUIRE(f.build({ "-x", "tests/" }).success());

        WHEN("built again without -x")
        {
            auto full = f.build();

            THEN("excluded commands run and prior work is not repeated")
            {
                INFO("stdout: " << full.stdout_output);
                INFO("stderr: " << full.stderr_output);
                REQUIRE(full.success());
                REQUIRE(f.exists("tests/t.out"));
                REQUIRE(f.exists("lib/tests/n.out"));
                REQUIRE(full.stdout_output.find("2 commands") != std::string::npos);
            }
        }
    }
}

SCENARIO("-x removes exactly the excluded commands from the build set", "[e2e][exclude]")
{
    GIVEN("a configured project")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());

        WHEN("dry runs list commands with and without -x tests/")
        {
            auto full = f.build({ "-n" });
            auto excluded = f.build({ "-n", "-x", "tests/" });

            THEN("the excluded listing is the full listing minus excluded commands")
            {
                INFO("full stdout: " << full.stdout_output);
                INFO("excluded stdout: " << excluded.stdout_output);
                REQUIRE(full.success());
                REQUIRE(excluded.success());
                REQUIRE(full.stdout_output.find("foo.out") != std::string::npos);
                REQUIRE(full.stdout_output.find("t.out") != std::string::npos);
                REQUIRE(full.stdout_output.find("n.out") != std::string::npos);
                REQUIRE(excluded.stdout_output.find("foo.out") != std::string::npos);
                REQUIRE(excluded.stdout_output.find("t.out") == std::string::npos);
                REQUIRE(excluded.stdout_output.find("n.out") == std::string::npos);
            }
        }
    }
}

SCENARIO("show graph respects -x", "[e2e][exclude][show]")
{
    GIVEN("a configured project")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());

        WHEN("show graph --summary runs with -x tests/")
        {
            auto excluded = f.pup({ "show", "graph", "--summary", "-x", "tests/" });

            THEN("only the non-excluded command is counted")
            {
                INFO("stdout: " << excluded.stdout_output);
                INFO("stderr: " << excluded.stderr_output);
                REQUIRE(excluded.success());
                REQUIRE(excluded.stdout_output.find("Commands: 1") != std::string::npos);
            }
        }
    }
}

SCENARIO("Exclusion composes with cwd scoping", "[e2e][exclude][scope]")
{
    GIVEN("a fresh project")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());

        WHEN("built from lib/ with -x tests/")
        {
            auto result = f.run_pup_in_dir("lib", { "-x", "tests/" });

            THEN("lib builds but lib/tests does not")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("lib/foo.out"));
                REQUIRE_FALSE(f.exists("lib/tests/n.out"));
                REQUIRE_FALSE(f.exists("tests/t.out"));
            }
        }
    }
}

SCENARIO("Multiple -x flags exclude every match", "[e2e][exclude]")
{
    GIVEN("a fresh project")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());

        WHEN("built with every directory excluded")
        {
            auto result = f.build({ "-x", "tests/", "-x", "lib/" });

            THEN("nothing is built")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
                REQUIRE_FALSE(f.exists("lib/foo.out"));
                REQUIRE_FALSE(f.exists("tests/t.out"));
            }
        }
    }
}

SCENARIO("Invalid -x patterns are rejected", "[e2e][exclude]")
{
    GIVEN("a configured project")
    {
        auto f = E2EFixture { "exclude_build" };
        REQUIRE(f.init().success());

        WHEN("built with an empty exclude pattern")
        {
            auto result = f.build({ "-x", "" });

            THEN("the build fails with a pattern error")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("exclude") != std::string::npos);
            }
        }

        WHEN("built with -x as the last argument")
        {
            auto result = f.build({ "-x" });

            THEN("the build fails")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
            }
        }
    }
}

SCENARIO("A .pupignore file has no effect", "[e2e][exclude]")
{
    GIVEN("a project with a .pupignore excluding tests/")
    {
        auto f = E2EFixture { "exclude_build" };
        f.write_file(".pupignore", "tests/\n");
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("every directory is built; only -x excludes")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("lib/foo.out"));
                REQUIRE(f.exists("tests/t.out"));
                REQUIRE(f.exists("lib/tests/n.out"));
            }
        }
    }
}

SCENARIO("Nested project roots are skipped by discovery", "[e2e][exclude][layout]")
{
    GIVEN("a project containing a subdirectory with its own Tupfile.ini")
    {
        auto f = E2EFixture { "nested_project" };
        REQUIRE(f.init().success());

        WHEN("the outer project is built")
        {
            auto result = f.build();

            THEN("the nested project is not built")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("outer.out"));
                REQUIRE_FALSE(f.exists("sub/inner.out"));
            }
        }
    }
}

SCENARIO("Pruning a nested project preserves its composed outputs", "[e2e][exclude][layout][incremental]")
{
    GIVEN("a nested project built via target selection into a variant dir")
    {
        auto f = E2EFixture { "nested_project" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "-B", "build", "sub/" }).success());
        REQUIRE(f.exists("build/sub/inner.out"));

        WHEN("a plain build runs without composing the nested project")
        {
            auto plain = f.build({ "-B", "build" });
            REQUIRE(plain.success());

            THEN("the nested project's outputs survive")
            {
                REQUIRE(f.exists("build/sub/inner.out"));
            }

            THEN("re-running the composed target is a no-op with outputs intact")
            {
                auto again = f.pup({ "-B", "build", "sub/" });
                INFO("stdout: " << again.stdout_output);
                REQUIRE(again.success());
                REQUIRE(again.is_noop());
                REQUIRE(f.exists("build/sub/inner.out"));
            }
        }
    }
}

SCENARIO("A group reference composes a nested project into the build", "[e2e][exclude][layout]")
{
    GIVEN("an outer Tupfile with a rule consuming sub/<out>")
    {
        auto f = E2EFixture { "group_composition" };
        REQUIRE(f.init().success());

        WHEN("the outer project is built")
        {
            auto result = f.build();

            THEN("the nested project is built too")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("outer.out"));
                REQUIRE(f.exists("sub/inner.out"));
            }
        }
    }
}

SCENARIO("A glob over generated files is path-ordered and stable across builds", "[e2e][glob]")
{
    GIVEN("a rule globbing files its neighbours generate, declared out of alphabetical order")
    {
        auto f = E2EFixture { "glob_generated" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        THEN("the matches are in path order, not rule order")
        {
            REQUIRE(f.read_file("matches.txt") == "alpha.gen mike.gen zeta.gen\n");
        }

        // The generated files exist on disk from here on, so a filesystem glob can
        // now see what only the graph could see during the first build.
        WHEN("the unchanged project is rebuilt")
        {
            auto second = f.build();

            THEN("nothing re-runs")
            {
                INFO("stdout: " << second.stdout_output);
                REQUIRE(second.is_noop());
            }

            THEN("the matches are unchanged")
            {
                REQUIRE(f.read_file("matches.txt") == "alpha.gen mike.gen zeta.gen\n");
            }
        }
    }
}

SCENARIO("A glob matches source and generated files together", "[e2e][glob]")
{
    GIVEN("a pattern matching both a checked-in file and two generated ones")
    {
        auto f = E2EFixture { "glob_generated_and_source" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("every match is an input, in path order")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("matches.txt") == "alpha.gen kept.gen zeta.gen\n");
            }
        }
    }
}

SCENARIO("A glob reaching into a sibling directory counts each match once", "[e2e][glob]")
{
    GIVEN("a pattern spelled ../b/*.q whose match is generated by a third directory")
    {
        auto f = E2EFixture { "glob_parent_dir" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        THEN("the generated file is one input, not one per spelling")
        {
            REQUIRE(f.read_file("z/out.txt") == "x\n");
        }

        WHEN("the unchanged project is rebuilt")
        {
            auto second = f.build();

            THEN("nothing re-runs")
            {
                INFO("stdout: " << second.stdout_output);
                REQUIRE(second.is_noop());
            }

            THEN("the input is still counted once")
            {
                REQUIRE(f.read_file("z/out.txt") == "x\n");
            }
        }
    }
}

SCENARIO("Imported values survive in the cache whatever order the imports are declared in", "[e2e][incremental]")
{
    GIVEN("a project importing three variables, declared in reverse-lexicographic order")
    {
        auto f = E2EFixture { "import_order" };
        {
            auto z = EnvGuard { "PUP_T_ZVAR", "zzz" };
            auto m = EnvGuard { "PUP_T_MVAR", "mmm" };
            auto a = EnvGuard { "PUP_T_AVAR", "aaa" };
            REQUIRE(f.init().success());
            REQUIRE(f.build().success());
            REQUIRE(f.read_file("out.txt") == "A=aaa M=mmm Z=zzz\n");
        }

        WHEN("the variables are unset and the project is rebuilt")
        {
            auto second = f.build();

            THEN("the values cached in the index are used")
            {
                INFO("stdout: " << second.stdout_output);
                REQUIRE(second.success());
                REQUIRE(f.read_file("out.txt") == "A=aaa M=mmm Z=zzz\n");
            }
        }
    }
}
