// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"

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
                auto modified = f.read_file("Tupfile");
                auto pos = modified.find("VERSION=1");
                REQUIRE(pos != std::string::npos);
                modified.replace(pos, 9, "VERSION=2");
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

SCENARIO("Config file changes trigger rebuild", "[e2e][incremental]")
{
    GIVEN("a project with tup.config")
    {
        auto f = E2EFixture { "config_change" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_OPT=1\n");
        REQUIRE(f.init().success());
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
        REQUIRE(f.init().success());
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
        REQUIRE(f.init().success());
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

        WHEN("a library file is modified and build runs from lib/")
        {
            f.append_file("lib/foo.c", "// modified\n");
            auto result = f.run_pup_in_dir("lib", { "-v" });

            THEN("both the library and dependent app are rebuilt")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("foo.o") != std::string::npos);
                REQUIRE(result.stdout_output.find("app") != std::string::npos);
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

SCENARIO("Scoped build without -a ignores upstream deps (mm behavior)", "[e2e][incremental][scope]")
{
    GIVEN("a project with shared include directory")
    {
        auto f = E2EFixture { "scoped_upstream" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("an upstream dependency (header) is modified and scoped build runs WITHOUT -a")
        {
            f.write_file("include/header.h", "#define VALUE 100\n");
            auto result = f.build({ "lib", "-v" });

            THEN("the build is a no-op (upstream change ignored)")
            {
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
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

SCENARIO("Variant builds with tup.config", "[e2e][variant]")
{
    GIVEN("a variant project with config")
    {
        auto f = E2EFixture { "variant" };
        f.mkdir("build");
        f.write_file("build/tup.config", "# Variant config\nCONFIG_VARIANT=build\n");
        REQUIRE(f.init().success());

        WHEN("built with --variant=build")
        {
            auto result = f.build({ "--variant=build" });

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

        REQUIRE(f.init().success());

        WHEN("built with --variant=build")
        {
            auto result = f.build({ "--variant=build" });

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

SCENARIO("Pupignore test via shell fixture", "[e2e][shell]")
{
    WHEN("the pupignore shell fixture runs")
    {
        auto result = run_shell_fixture("pupignore");

        THEN("ignored directories are skipped")
        {
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

SCENARIO("Show script generates shell build script", "[e2e][show]")
{
    GIVEN("a simple C project")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());

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
        REQUIRE(f.init().success());
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

            THEN("build directory is auto-detected via tup.config")
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

        WHEN("show graph --all-deps is run without -B")
        {
            auto result = f.pup({ "show", "graph", "--summary", "--all-deps" });

            THEN("build directory is detected")
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

        WHEN("clean is run without -B")
        {
            auto result = f.clean();

            THEN("it finds build/.pup and cleans successfully")
            {
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("no index found") == std::string::npos);
            }
        }

        WHEN("show graph --all-deps is run without -B")
        {
            auto result = f.pup({ "show", "graph", "--summary", "--all-deps" });

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

SCENARIO("Multi-variant auto-detection", "[e2e][multi-variant]")
{
    GIVEN("a project with multiple variant directories")
    {
        auto f = E2EFixture { "multi_variant" };

        // Create two variant directories with tup.config
        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");

        REQUIRE(f.init().success());

        WHEN("pup is run from project root without -B")
        {
            auto result = f.build();

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
            (void)f.build();
            auto result = f.build();

            THEN("nothing is rebuilt")
            {
                REQUIRE(result.is_noop());
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

        REQUIRE(f.init().success());

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

        REQUIRE(f.init().success());

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

        REQUIRE(f.init().success());

        WHEN("pup -v is run with multiple variants")
        {
            auto result = f.build({ "-v" });

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
        REQUIRE(f.init().success());

        REQUIRE(f.build().success());
        REQUIRE(f.exists("build-debug/hello"));
        REQUIRE(f.exists("build-release/hello"));

        WHEN("pup clean is run from project root")
        {
            auto result = f.clean();

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
        REQUIRE(f.init().success());

        REQUIRE(f.build().success());
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
        REQUIRE(f.init().success());

        WHEN("pup parse is run from project root")
        {
            auto result = f.parse();

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

        REQUIRE(f.init().success());

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

        REQUIRE(f.init().success());

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

        REQUIRE(f.init().success());

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

            THEN("an error is returned")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("not found") != std::string::npos);
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

        REQUIRE(f.init().success());

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

        REQUIRE(f.init().success());

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

        REQUIRE(f.init().success());

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

        REQUIRE(f.init().success());

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
// Scoped tup.config tests
// =============================================================================

SCENARIO("Subdir uses its own tup.config", "[e2e][scoped-config]")
{
    GIVEN("a project with sub/Tupfile using @(SUB_VAR)")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_ROOT_VAR=from_root\n");
        f.write_file("build/sub/tup.config", "CONFIG_SUB_VAR=from_sub\n");
        REQUIRE(f.init().success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(SUB_VAR) resolves to 'from_sub'")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/sub.txt") == "from_sub\n");
            }

            THEN("@(ROOT_VAR) in sub/ resolves to '' (not inherited)")
            {
                REQUIRE(f.read_file("build/sub/root_from_sub.txt") == "\n");
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
        REQUIRE(f.init().success());

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
        REQUIRE(f.init().success());

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

SCENARIO("Empty subdir config blocks inheritance", "[e2e][scoped-config]")
{
    GIVEN("a project with sub/Tupfile using @(ROOT_VAR)")
    {
        auto f = E2EFixture { "scoped_config" };
        f.mkdir("build/sub");
        f.write_file("build/tup.config", "CONFIG_ROOT_VAR=from_root\n");
        f.write_file("build/sub/tup.config", ""); // Empty config blocks lookup
        REQUIRE(f.init().success());

        WHEN("pup builds the project")
        {
            auto result = f.build({ "-B", "build" });

            THEN("@(ROOT_VAR) in sub/ resolves to '' (empty config blocks lookup)")
            {
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/sub/root_from_sub.txt") == "\n");
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
        REQUIRE(f.init().success());

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("sub/tup.config is created")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/sub/tup.config"));
            }

            THEN("out.txt is NOT created (non-config rule skipped)")
            {
                REQUIRE_FALSE(f.exists("build/sub/out.txt"));
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
        REQUIRE(f.init().success());

        WHEN("pup configure runs")
        {
            auto result = f.pup({ "configure", "-B", "build" });

            THEN("@(MACHINE) resolves to 'board-xyz' and config is generated")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("build/sub/tup.config"));
                auto content = f.read_file("build/sub/tup.config");
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
        REQUIRE(f.init().success());
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
    GIVEN("a project where configs/ outputs sub/tup.config")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.mkdir("build");
        f.write_file("build/tup.config", "CONFIG_MACHINE=hello-world\n");
        REQUIRE(f.init().success());

        WHEN("pup configure runs, then pup runs")
        {
            auto configure_result = f.pup({ "configure", "-B", "build" });
            REQUIRE(configure_result.success());

            auto build_result = f.build({ "-B", "build" });

            THEN("out.txt contains the message from generated config")
            {
                REQUIRE(build_result.success());
                REQUIRE(f.exists("build/sub/out.txt"));
                auto content = f.read_file("build/sub/out.txt");
                REQUIRE(content.find("hello-world") != std::string::npos);
            }
        }
    }
}

SCENARIO("Configure handles config rule depending on non-config rule", "[e2e][configure][deps]")
{
    // Bug: build_subset only runs config-output rules, ignoring their dependencies.
    // If a config rule depends on an intermediate file produced by a non-config rule,
    // the dependency is not run, causing the config rule to fail.

    GIVEN("a project where config rule depends on intermediate file")
    {
        auto f = E2EFixture { "configure_deps" };
        f.mkdir("build");
        f.write_file("build/tup.config", "");
        REQUIRE(f.init().success());

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
                REQUIRE(f.exists("build/sub/tup.config"));
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
                REQUIRE(f.exists("build/sub/tup.config"));
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
