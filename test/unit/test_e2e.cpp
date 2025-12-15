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
            (void)f.build(); // first build
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
                f.write_file("config.h",
                    "#ifndef CONFIG_H\n"
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
        REQUIRE(f.init().success());
        f.mkdir("build");
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
        REQUIRE(f.init().success());
        f.mkdir("build");
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
    GIVEN("an initialized project without builds")
    {
        auto f = E2EFixture { "distclean_no_index" };
        REQUIRE(f.init().success());
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
        REQUIRE(f.init().success());
        f.mkdir("build");
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
        REQUIRE(f.init().success());
        f.mkdir("build");
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
    GIVEN("an initialized out_of_tree project")
    {
        auto f = E2EFixture { "out_of_tree" };
        REQUIRE(f.init().success());
        f.mkdir("build");

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
