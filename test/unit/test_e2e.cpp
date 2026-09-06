// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/format.hpp"
#include "pup/index/reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pup::test;

// The assertion most incremental scenarios rest on, so it has to be exact: a substring test
// for "0 commands" also accepts every multiple of ten, and a quiescence check that can pass
// while a build ran hides the very defects those scenarios exist to catch (#234).
TEST_CASE("is_noop accepts only a build that ran nothing", "[e2e-fixture]")
{
    auto result_with = [](std::string out) { return PupResult { .exit_code = 0, .stdout_output = std::move(out), .stderr_output = {} }; };

    SECTION("a build that ran nothing is a no-op")
    {
        REQUIRE(result_with("[.] Nothing to do (up to date).\n").is_noop());
        REQUIRE(result_with("[.] Build completed: 0 commands in 3ms\n").is_noop());
    }

    SECTION("a build that ran commands is not, however many it ran")
    {
        REQUIRE_FALSE(result_with("[.] Build completed: 1 commands in 3ms\n").is_noop());
        REQUIRE_FALSE(result_with("[.] Build completed: 10 commands in 3ms\n").is_noop());
        REQUIRE_FALSE(result_with("[.] Build completed: 20 commands in 3ms\n").is_noop());
        REQUIRE_FALSE(result_with("[.] Build completed: 100 commands in 3ms\n").is_noop());
        REQUIRE_FALSE(result_with("[.] Build completed: 30 commands (2 failed) in 3ms\n").is_noop());
    }
}

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

SCENARIO("A rule's display text stands in for the command in build output", "[e2e][build][display]")
{
    GIVEN("a rule carrying a ^ text ^ display annotation")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> ^ GREET the-file^ echo hi > %o |> out.txt\n");
        REQUIRE(f.init().success());

        WHEN("the build reports each command it runs")
        {
            auto result = f.build({ "-v" });
            REQUIRE(result.success());

            THEN("the annotation is what appears, in place of the command text")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("GREET the-file") != std::string::npos);
                REQUIRE(result.stdout_output.find("echo hi >") == std::string::npos);
            }
        }
    }
}

SCENARIO("Percent flags inside display text expand against the rule", "[e2e][build][display]")
{
    GIVEN("a rule whose display annotation names its output and stem")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("in.src", "x\n");
        f.write_file("Tupfile", ": foreach *.src |> ^ CC %B -> %o^ cp %f %o |> %B.obj\n");
        REQUIRE(f.init().success());

        WHEN("the build reports the command it runs")
        {
            auto result = f.build({ "-v" });
            REQUIRE(result.success());

            THEN("the flags render as the rule's own stem and output")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("CC in -> in.obj") != std::string::npos);
            }
        }
    }
}

SCENARIO("An unterminated caret is a parse error, not a shell error", "[e2e][build][display]")
{
    // Upstream rejects this at parse time; falling through left the ^ in the command text,
    // so the rule died mid-build as "sh: ^: not found" — blamed on the tool, not the typo (#217).
    GIVEN("a rule whose display annotation is never closed")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        REQUIRE(f.init().success());
        f.write_file("Tupfile", ": |> ^ CC out.txt echo hi > %o |> out.txt\n");

        WHEN("the project is parsed")
        {
            auto result = f.parse();

            THEN("parsing fails and says the caret was never closed")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("not found") == std::string::npos);
                REQUIRE(combined.find("^") != std::string::npos);
            }
        }
    }
}

SCENARIO("An upstream caret flag is rejected, not rendered as a label", "[e2e][build][display]")
{
    // tup reads the non-space run after ^ as flags (t, o); putup implements neither, and
    // printing "t" as the rule's label honours nothing and refuses nothing (#217).
    GIVEN("a rule using upstream's caret-flag form")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        REQUIRE(f.init().success());
        f.write_file("Tupfile", ": |> ^t^ echo hi > %o |> out.txt\n");

        WHEN("the project is parsed")
        {
            auto result = f.parse();

            THEN("it is refused rather than shown as display text")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
            }
        }
    }
}

SCENARIO("A failing command is reported by its command line, not its display", "[e2e][build][display]")
{
    // The display names the step, not what broke, and this is the only line a build prints of what actually ran.
    GIVEN("an annotated rule whose command fails")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> ^ OOPS^ sh -c \"echo boom >&2; exit 1\" > %o |> out.txt\n");
        REQUIRE(f.init().success());

        WHEN("the build runs and the command fails")
        {
            auto result = f.build();
            REQUIRE_FALSE(result.success());

            THEN("the failure names the command, and the tool's own output survives")
            {
                INFO("stderr: " << result.stderr_output);
                INFO("stdout: " << result.stdout_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("FAILED: sh -c") != std::string::npos);
                REQUIRE(combined.find("boom") != std::string::npos);
            }
        }
    }
}

SCENARIO("A failing config rule is reported by its command line, not its display", "[e2e][configure][display]")
{
    GIVEN("an annotated config-generating rule whose command fails")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.write_file("Tupfile", ": configs/board.config |> ^ GENCONF^ sh -c \"echo boom >&2; exit 1\" > %o |> tup.config\n");
        f.write_file("configs/Tupfile", "# no rules\n");
        f.write_file("sub/Tupfile", "# no rules\n");

        WHEN("configure runs and the rule fails")
        {
            auto result = f.pup({ "configure" });
            REQUIRE_FALSE(result.success());

            THEN("the failure names the command, and the tool's own output survives")
            {
                INFO("stderr: " << result.stderr_output);
                INFO("stdout: " << result.stdout_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("FAILED: sh -c") != std::string::npos);
                REQUIRE(combined.find("boom") != std::string::npos);
            }
        }
    }
}

SCENARIO("A bang macro's display wins over one written on the rule", "[e2e][build][display][bang]")
{
    // Outputs and groups resolve rule-over-macro; display is the one field that goes the other way, because the macro owns the command the display names.
    GIVEN("a macro carrying a display, applied by a rule that also carries one")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("in.src", "x\n");
        f.write_file("Tupfile", "!cp = |> ^ MACRO %o^ cp %f %o |>\n: foreach *.src |> ^ RULE %o^ !cp |> %B.obj\n");
        REQUIRE(f.init().success());

        WHEN("the build reports the command it runs")
        {
            auto result = f.build({ "-v" });
            REQUIRE(result.success());

            THEN("the macro's display is used and the rule's is discarded")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("MACRO in.obj") != std::string::npos);
                REQUIRE(result.stdout_output.find("RULE") == std::string::npos);
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

SCENARIO("A build whose command outgrows the record's string entry still converges", "[e2e][build][groups][incremental]")
{
    GIVEN("a group whose expanded member list renders past 64 KB in one command")
    {
        auto f = E2EFixture { "glob_mixed_space" };

        auto const pad = std::string(220, 'x');
        auto tupfile = std::string {};
        for (auto i = 0; i < 300; ++i) {
            tupfile += ": |> touch %o |> obj_" + std::to_string(i) + "_" + pad + ".o <objs>\n";
        }
        tupfile += ": | <objs> |> echo %<objs> > %o |> linked.txt\n";
        f.write_file("Tupfile", tupfile);
        REQUIRE(f.init().success());

        WHEN("it is built and then built again")
        {
            auto first = f.build({ "-j4" });
            auto second = f.build({ "-j4" });

            THEN("the first build records what it ran")
            {
                INFO("stdout: " << first.stdout_output << "\nstderr: " << first.stderr_output);
                REQUIRE(first.success());
                REQUIRE(f.read_file("linked.txt").size() > 65535);
            }

            THEN("the second build has nothing to do")
            {
                INFO("stdout: " << second.stdout_output << "\nstderr: " << second.stderr_output);
                REQUIRE(second.is_noop());
            }
        }
    }
}

SCENARIO("A carried-forward record stops claiming currency whatever the operand's position", "[e2e][incremental][scope]")
{
    GIVEN("a command with 300 outputs, one of them consumed from another directory")
    {
        auto f = E2EFixture { "scoped_stale" };

        auto outputs = std::string {};
        for (auto i = 1; i <= 300; ++i) {
            outputs += " out_" + std::to_string(i) + ".txt";
        }
        f.write_file("big/Tupfile", ": |> sh -c 'for i in `seq 1 300`; do echo x > out_$i.txt; done' |>" + outputs + "\n");
        f.write_file("use/Tupfile", ": ../big/out_1.txt ../big/out_300.txt |> cat %f > %o |> combined.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("the 300th output changes behind the build and a build scoped elsewhere carries the record")
        {
            f.write_file("big/out_300.txt", "TAMPERED\n");
            REQUIRE(f.build({ "use/" }).success());

            THEN("the carried record stops claiming that command's outputs are current")
            {
                auto shown = f.pup({ "show", "index" });
                INFO("stdout: " << shown.stdout_output);
                REQUIRE(shown.success());
                REQUIRE(shown.stdout_output.find("[big]  must_rerun") != std::string::npos);
            }
        }

        WHEN("the 1st output changes instead")
        {
            f.write_file("big/out_1.txt", "TAMPERED\n");
            REQUIRE(f.build({ "use/" }).success());

            THEN("the carried record stops claiming that command's outputs are current")
            {
                auto shown = f.pup({ "show", "index" });
                INFO("stdout: " << shown.stdout_output);
                REQUIRE(shown.success());
                REQUIRE(shown.stdout_output.find("[big]  must_rerun") != std::string::npos);
            }
        }
    }
}

SCENARIO("A scoped build keeps the record of who owns a generated file it did not parse", "[e2e][incremental][scope]")
{
    GIVEN("a generated file in one directory consumed by a rule in another")
    {
        auto f = E2EFixture { "scoped_stale" };

        f.write_file("big/Tupfile", ": |> sh -c 'for i in `seq 1 5`; do echo x > out_$i.txt; done' |> out_1.txt out_2.txt out_3.txt out_4.txt out_5.txt\n");
        f.write_file("use/extra.txt", "v1\n");
        f.write_file("use/Tupfile", ": extra.txt ../big/out_1.txt |> cat %f > %o |> combined.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("a build scoped to the consumer runs, with nothing in the producing directory touched")
        {
            f.write_file("use/extra.txt", "v2\n");
            REQUIRE(f.build({ "use/" }).success());

            THEN("the record still calls the consumed file generated, so clean removes it")
            {
                auto cleaned = f.clean();
                INFO("stdout: " << cleaned.stdout_output);
                REQUIRE(cleaned.success());
                REQUIRE_FALSE(f.exists("big/out_1.txt"));
            }

            THEN("a later full build still runs")
            {
                auto full = f.build();
                INFO("stdout: " << full.stdout_output << "\nstderr: " << full.stderr_output);
                REQUIRE(full.success());
            }
        }

        WHEN("the consumed file is overwritten by hand before that scoped build")
        {
            f.write_file("big/out_1.txt", "overwritten\n");
            f.write_file("use/extra.txt", "v2\n");
            REQUIRE(f.build({ "use/" }).success());

            THEN("overwriting an output does not transfer its ownership, so clean still removes it")
            {
                auto cleaned = f.clean();
                INFO("stdout: " << cleaned.stdout_output);
                REQUIRE(cleaned.success());
                REQUIRE_FALSE(f.exists("big/out_1.txt"));
            }
        }
    }
}

SCENARIO("A scoped build keeps the record of who owns a generated header it discovered", "[e2e][incremental][scope][implicit]")
{
    GIVEN("a compile whose discovered dependency is generated in another directory")
    {
        auto f = E2EFixture { "scoped_stale" };

        f.write_file("gen/Tupfile", ": |> echo '#define VERSION 1' > %o |> header.h\n");
        f.write_file("src/main.c", "#include \"header.h\"\nint main(void) { return VERSION; }\n");
        f.write_file("src/Tupfile", ": main.c |> cc -MD -I../gen -c %f -o %o |> main.o\n");
        f.write_file("other/keep.txt", "v1\n");
        f.write_file("other/Tupfile", ": keep.txt |> cat %f > %o |> other.out\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("gen/header.h"));

        WHEN("the compile re-runs under a scope that excludes the generator")
        {
            f.write_file("src/main.c", "#include \"header.h\"\nint main(void) { return VERSION + 0; }\n");
            REQUIRE(f.build({ "src/" }).success());

            THEN("the record still calls the discovered header generated, so clean removes it")
            {
                auto cleaned = f.clean();
                INFO("stdout: " << cleaned.stdout_output);
                REQUIRE(cleaned.success());
                REQUIRE_FALSE(f.exists("gen/header.h"));
            }
        }

        WHEN("a scope that excludes both the generator and the compile carries the edge forward")
        {
            f.write_file("other/keep.txt", "v2\n");
            REQUIRE(f.build({ "other/" }).success());

            THEN("the record still calls the carried header generated, so clean removes it")
            {
                auto cleaned = f.clean();
                INFO("stdout: " << cleaned.stdout_output);
                REQUIRE(cleaned.success());
                REQUIRE_FALSE(f.exists("gen/header.h"));
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

SCENARIO("A Tupfile edit that changes no command re-runs nothing", "[e2e][incremental][identity]")
{
    // Complement of "Editing an output-less command re-runs it". The rule needs an output: the Sticky route propagates a command's outputs, so an output-less one cannot exhibit this at all (#225).
    GIVEN("a built project whose rebuild is stable")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> echo hi > %o |> out.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("a comment is added to the Tupfile, leaving every command's text identical")
        {
            f.write_file("Tupfile", "# a comment changes no command\n: |> echo hi > %o |> out.txt\n");
            auto result = f.build();

            THEN("nothing re-runs")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
            }
        }
    }
}

SCENARIO("A rebuild reason names a command that carries no display annotation", "[e2e][display][incremental]")
{
    GIVEN("a built project whose rule has no ^ ^ annotation")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> echo one > %o |> out.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("the rule's command text changes")
        {
            f.write_file("Tupfile", ": |> echo two > %o |> out.txt\n");
            auto result = f.build({ "-v" });
            REQUIRE(result.success());

            THEN("the reason names the command, rather than trailing off after the colon")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("Changed command: echo two > out.txt") != std::string::npos);
                REQUIRE(result.stdout_output.find("Changed command: \n") == std::string::npos);
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

SCENARIO("A build record that is not putup's own is refused out loud", "[e2e][index]")
{
    GIVEN("a built project whose record is quiescent")
    {
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("one byte inside the record is corrupted")
        {
            auto const index_path = f.workdir() / ".pup" / "index";
            auto bytes = std::string {};
            {
                auto in = std::ifstream { index_path, std::ios::binary };
                bytes.assign(std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {});
            }
            REQUIRE(bytes.size() > 128);

            // Mid-file: past the header putup already validates, ahead of the footer, so nothing
            // but the checksum can notice.
            auto const pos = bytes.size() / 2;
            bytes[pos] = static_cast<char>(bytes[pos] ^ 0x01);
            {
                auto out = std::ofstream { index_path, std::ios::binary | std::ios::trunc };
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }

            auto const result = f.build();

            THEN("the build names the cause instead of reading the record anyway")
            {
                REQUIRE(result.stderr_output.find("failed its checksum") != std::string::npos);
            }

            // With the record refused, nothing left says which files on disk this project
            // produced, and #291's rule is that putup does not guess -- so the build stops and
            // names them rather than treating its own outputs as checked-in sources.
            THEN("it refuses rather than claim outputs it can no longer prove are its own")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("cannot be read") != std::string::npos);
                REQUIRE(result.stderr_output.find("program") != std::string::npos);
            }

            THEN("deleting the files it named is enough to build again")
            {
                f.remove_file("main.o");
                f.remove_file("program");
                REQUIRE(f.build().success());
                REQUIRE(f.build().is_noop());
            }
        }
    }
}

SCENARIO("A build records one entry per path", "[e2e][index]")
{
    GIVEN("a project whose discovered headers live outside the source tree")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("the recorded index is read back")
        {
            auto const index = pup::index::read_index((f.workdir() / ".pup" / "index").string());
            REQUIRE(index.has_value());

            auto paths = std::vector<std::string_view> {};
            for (auto const& file : index->files()) {
                if (!pup::is_empty(file.path)) {
                    paths.push_back(pup::global_pool().get(file.path));
                }
            }
            std::sort(paths.begin(), paths.end());

            THEN("an absolute chain was walked, so the root case ran")
            {
                REQUIRE(std::ranges::any_of(paths, [](auto p) { return p.starts_with("/"); }));
            }

            // The directory walk creates an entry and registers it for the next lookup; register
            // anything but what it created and the next chain re-creates it (#325).
            THEN("no two entries answer to the same path")
            {
                auto const dup = std::adjacent_find(paths.begin(), paths.end());
                INFO("duplicated path: " << (dup == paths.end() ? std::string_view {} : *dup));
                REQUIRE(dup == paths.end());
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

SCENARIO("A dependency outside the source tree is recorded rather than dropped", "[e2e][incremental][implicit]")
{
    // The arms that drop a discovered dep sit above an else that keeps the out-of-tree ones as
    // absolute paths. Nothing pinned that, and #305 was filed on the assumption it drops them.
    GIVEN("a compile whose depfile names system headers")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": foreach *.c |> gcc -MD -MF %o.d -c %f -o %o |> %B.o\n");
        f.write_file("a.c", "#include <stdio.h>\nint a(void){return 0;}\n");

        WHEN("the project is built without -v")
        {
            auto built = f.build();

            THEN("it says nothing about dependencies it could not take")
            {
                INFO("stdout: " << built.stdout_output);
                INFO("stderr: " << built.stderr_output);
                REQUIRE(built.success());
                REQUIRE(built.stderr_output.find("Skipping dependency") == std::string::npos);
                REQUIRE(built.stderr_output.find("Cannot relativize") == std::string::npos);
            }

            THEN("the headers outside the tree are in the record, by absolute path")
            {
                auto shown = f.pup({ "show", "index" });
                INFO("stdout: " << shown.stdout_output);
                REQUIRE(shown.success());
                REQUIRE(shown.stdout_output.find("implicit: /") != std::string::npos);
            }
        }
    }
}

SCENARIO("A changed header re-runs the output-less command that read it", "[e2e][incremental][implicit]")
{
    // Routing a discovered dep pushes the reading command's outputs, so a command with none was
    // reached and then dropped. A compile gate is the shape that has no outputs on purpose (#228).
    GIVEN("an output-less compile gate that reads a header")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("bar.h", "#define VAL 1\n");
        f.write_file("foo.c", "#include \"bar.h\"\nint f(void) { return VAL; }\n");
        f.write_file("Tupfile", ": foo.c |> gcc -c %f -o /dev/null |>\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("the header changes")
        {
            f.write_file("bar.h", "#define VAL 2\n");
            auto result = f.build();

            THEN("the gate runs again rather than reporting the tree up to date")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }

            AND_WHEN("the header changes a second time")
            {
                REQUIRE(f.build().is_noop());
                f.write_file("bar.h", "#define VAL 3\n");
                auto again = f.build();

                THEN("it runs again: one rebuild must not consume the dependency")
                {
                    INFO("stdout: " << again.stdout_output);
                    REQUIRE(again.success());
                    REQUIRE_FALSE(again.is_noop());
                }
            }
        }
    }
}

SCENARIO("A scoped build sees a declared input outside the scope change", "[e2e][incremental][scope]")
{
    // Detection skips out-of-scope files unless a bypass covers them, and the bypass admitted
    // Implicit and Sticky edges but not Normal — so a plainly declared source input was the one
    // kind of dependency a scoped build could not see (#200).
    GIVEN("a rule in a subdirectory declaring a source file above it")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("sub");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("outer.txt", "outer-v1\n");
        f.write_file("sub/inner.txt", "inner-v1\n");
        f.write_file("sub/Tupfile", ": inner.txt ../outer.txt |> cat %f > %o |> combined.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build({ "sub/" }).success());
        REQUIRE(f.read_file("sub/combined.txt") == "inner-v1\nouter-v1\n");
        REQUIRE(f.build({ "sub/" }).is_noop());

        WHEN("the out-of-scope source changes and the same scope is rebuilt")
        {
            f.write_file("outer.txt", "outer-v2\n");
            auto result = f.build({ "sub/" });

            THEN("the rule re-runs against the new content")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("sub/combined.txt") == "inner-v1\nouter-v2\n");
            }
        }
    }
}

SCENARIO("A command that raced its discovered dependency runs again", "[e2e][incremental][implicit]")
{
    // Nothing orders a consumer against a producer it only discovers, so it can read the file
    // before it exists. The dep is then recorded from a post-run stat — as already satisfied —
    // and no later build re-runs it, leaving the output permanently wrong (#274).
    GIVEN("a consumer that discovers a generated file with nothing ordering the two")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": |> sleep 1; echo produced > %o |> p.txt\n");
        f.write_file("b/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "b/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../a/p.txt ]; then cat ../a/p.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../a/p.txt\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("a/p.txt"));

        WHEN("the project is built again")
        {
            REQUIRE(f.build().success());

            THEN("the consumer has caught up with the dependency it raced")
            {
                INFO("c.o: " << f.read_file("b/c.o"));
                REQUIRE(f.read_file("b/c.o") == "produced\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A consumer ordered through a sibling output is not taxed for discovering the other", "[e2e][incremental][implicit]")
{
    // Codegen emitting one declared and one discovered output: the consumer is ordered through
    // the declared one, so it cannot have raced the discovered one. Marking it anyway doubles
    // every codegen rebuild — the shape a two-hop ordering check cannot see (#274).
    GIVEN("a generator producing a source the consumer declares and a header it only discovers")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": gen.sh |> sh gen.sh |> gen.c gen.h\n"
            ": gen.c |> sh build.sh %f %o |> gen.o\n");
        f.write_file("gen.sh", "echo 'int v(void);' > gen.h\necho 'int v(void){return 1;}' > gen.c\n");
        f.write_file("build.sh", "cat \"$1\" > \"$2\"\nprintf '%s: gen.h\\n' \"$2\" > \"${2%.o}.d\"\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("the generator's input changes, re-running it and the consumer")
        {
            f.write_file("gen.sh", "echo 'int v(void);' > gen.h\necho 'int v(void){return 2;}' > gen.c\n");
            REQUIRE(f.build().success());

            THEN("the build after that does nothing")
            {
                auto settled = f.build();
                INFO("stdout: " << settled.stdout_output);
                REQUIRE(settled.success());
                REQUIRE(settled.is_noop());
            }
        }
    }
}

SCENARIO("A discovered dependency orders its consumer on a later build", "[e2e][incremental][implicit]")
{
    // A discovery is index-only, so it orders nothing: every later build that runs both the
    // producer and the consumer races them again, and the consumer is taxed with an extra run
    // to catch up. The previous build's discovery is what the scheduler orders by now (#276).
    GIVEN("a settled project whose consumer only discovers what the other produces")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": seed.txt |> sleep 1; cat seed.txt > %o |> p.txt\n");
        f.write_file("a/seed.txt", "v1\n");
        f.write_file("b/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "b/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../a/p.txt ]; then cat ../a/p.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../a/p.txt\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("both commands are re-run by their own inputs changing")
        {
            f.write_file("a/seed.txt", "v9\n");
            f.append_file("b/gen.sh", "# touched\n");
            auto rerun = f.build();
            REQUIRE(rerun.success());

            THEN("the consumer read what the producer wrote in that same build")
            {
                INFO("c.o: " << f.read_file("b/c.o"));
                REQUIRE(f.read_file("b/c.o") == "v9\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("nothing needs to catch up")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A recorded discovery that the rules now contradict does not stall the build", "[e2e][incremental][implicit]")
{
    // The ordering carried from the last build is stale by construction: the rules can since
    // have turned the discovered file's producer into a consumer of the discoverer's output.
    // The rules are this build's truth, so the contradiction retracts the carried ordering —
    // taking it as binding leaves both commands waiting for each other, and a scheduler with
    // nothing runnable and nothing running reports the build complete having run neither (#276).
    GIVEN("a settled project where one command discovers what the other produces")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": a.sh |> sh a.sh %o |> x.o\n"
            ": b.sh |> sh b.sh %o |> y.txt\n");
        f.write_file(
            "a.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f y.txt ]; then cat y.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: y.txt\\n' \"$out\" > \"$dep\"\n"
        );
        f.write_file("b.sh", "echo v1 > \"$1\"\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("the rules reverse the two, so the recorded discovery points the wrong way")
        {
            f.write_file("Tupfile",
                ": a.sh |> sh a.sh %o |> x.o\n"
                ": b.sh x.o |> sh b.sh %o |> y.txt\n");
            f.write_file(
                "a.sh",
                "out=$1\n"
                "dep=\"${out%.o}.d\"\n"
                "echo standalone > \"$out\"\n"
                "printf '%s: a.sh\\n' \"$out\" > \"$dep\"\n"
            );
            f.write_file("b.sh", "echo v2 > \"$1\"\n");
            auto reversed = f.build();

            THEN("both commands still run, in the order the rules give")
            {
                INFO("stdout: " << reversed.stdout_output);
                REQUIRE(reversed.success());
                REQUIRE(!reversed.is_noop());
                REQUIRE(f.read_file("x.o") == "standalone\n");
                REQUIRE(f.read_file("y.txt") == "v2\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A discovery whose producing rule is gone orders nothing", "[e2e][incremental][implicit]")
{
    // Ordering carried from the last build names a producer by the file it produces, so a rule
    // that has since stopped producing it names nothing and the consumer waits for no one (#276).
    GIVEN("a settled project whose consumer discovers a generated file")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": gen.sh |> sh gen.sh %o |> h.txt\n"
            ": c.sh |> sh c.sh %o |> c.o\n");
        f.write_file("gen.sh", "echo generated > \"$1\"\n");
        f.write_file(
            "c.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f h.txt ]; then\n"
            "  cat h.txt > \"$out\"\n"
            "  printf '%s: h.txt\\n' \"$out\" > \"$dep\"\n"
            "else\n"
            "  echo missing > \"$out\"\n"
            "  : > \"$dep\"\n"
            "fi\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("the rule producing the discovered file is removed")
        {
            f.write_file("Tupfile", ": c.sh |> sh c.sh %o |> c.o\n");
            auto removed = f.build();

            THEN("the consumer runs against the file's absence")
            {
                INFO("stdout: " << removed.stdout_output);
                REQUIRE(removed.success());
                REQUIRE(f.read_file("c.o") == "missing\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("Ordering the scheduler did not enforce does not excuse a race", "[e2e][incremental][implicit]")
{
    // A carried ordering is only real for a pair this build actually scheduled: the consumer may
    // be quiescent and absent from the job set, and then nothing enforced it. Crediting it anyway
    // lets a command reach its own excuse through the missing one's outputs, and the race it did
    // commit goes unmarked — the permanently wrong output #274 exists to prevent (#276).
    GIVEN("a settled project whose quiescent consumer bridges a producer to a third command")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("p");
        f.mkdir("c");
        f.mkdir("x");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("p/Tupfile", ": seed.txt |> sleep 1; cat seed.txt > %o |> d.txt\n");
        f.write_file("p/seed.txt", "v1\n");
        f.write_file("c/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "c/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../p/d.txt ]; then cat ../p/d.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../p/d.txt\\n' \"$out\" > \"$dep\"\n"
        );
        f.write_file("x/Tupfile", ": gen.sh ../c/c.o |> sh gen.sh %o |> x.o\n");
        f.write_file(
            "x/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "cat ../c/c.o > \"$out\"\n"
            ": > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("a third command starts reading the producer's file while the consumer stays put")
        {
            f.write_file("p/seed.txt", "v2\n");
            f.write_file(
                "x/gen.sh",
                "out=$1\n"
                "dep=\"${out%.o}.d\"\n"
                "cat ../p/d.txt > \"$out\"\n"
                "printf '%s: ../p/d.txt\\n' \"$out\" > \"$dep\"\n"
            );
            REQUIRE(f.build().success());

            THEN("the build after the race heals it")
            {
                auto heal = f.build();
                INFO("stdout: " << heal.stdout_output);
                INFO("x.o: " << f.read_file("x/x.o"));
                REQUIRE(heal.success());
                REQUIRE(f.read_file("x/x.o") == "v2\n");
            }

            AND_WHEN("it is built twice more")
            {
                REQUIRE(f.build().success());

                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A producer's own input change reaches the consumer that only discovered it", "[e2e][incremental][implicit]")
{
    // The affected cascade walks graph edges from the pre-build changed set, and a discovered
    // dependency has none; the file is not known changed until its producer has run, and the
    // index then stamps it from a post-run stat. So the consumer was never scheduled at all and
    // its output stayed wrong while the build reported the tree up to date (#277).
    GIVEN("a settled project whose consumer only discovers what the other produces")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": seed.txt |> sleep 1; cat seed.txt > %o |> p.txt\n");
        f.write_file("a/seed.txt", "v1\n");
        f.write_file("b/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "b/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../a/p.txt ]; then cat ../a/p.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../a/p.txt\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("only the producer's input changes")
        {
            f.write_file("a/seed.txt", "v4\n");
            auto rebuild = f.build();
            REQUIRE(rebuild.success());

            THEN("the consumer ran too, and after the producer")
            {
                INFO("stdout: " << rebuild.stdout_output);
                INFO("c.o: " << f.read_file("b/c.o"));
                REQUIRE(f.read_file("b/c.o") == "v4\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }

            AND_WHEN("the consumer stops reading the file and reports nothing")
            {
                f.write_file(
                    "b/gen.sh",
                    "out=$1\n"
                    "dep=\"${out%.o}.d\"\n"
                    "echo standalone > \"$out\"\n"
                    ": > \"$dep\"\n"
                );
                REQUIRE(f.build().success());
                REQUIRE(f.build().is_noop());

                THEN("the producer's next change reaches it no longer")
                {
                    f.write_file("a/seed.txt", "v5\n");
                    auto producer_only = f.build();
                    INFO("stdout: " << producer_only.stdout_output);
                    REQUIRE(producer_only.success());
                    REQUIRE(producer_only.stdout_output.find("Build completed: 1 commands") != std::string::npos);
                    REQUIRE(f.read_file("b/c.o") == "standalone\n");
                }
            }
        }
    }
}

SCENARIO("A producer's input change reaches a chain of discovered consumers", "[e2e][incremental][implicit]")
{
    // Routing a discovered consumer makes its own outputs change, so whatever discovered those
    // must follow. Expanding the recorded pairs once rather than inside the cascade's fixpoint
    // would reach the first consumer and stop (#277).
    GIVEN("a settled chain where each link only discovers the one before it")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.mkdir("c");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": seed.txt |> sleep 1; cat seed.txt > %o |> p.txt\n");
        f.write_file("a/seed.txt", "v1\n");
        f.write_file("b/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "b/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../a/p.txt ]; then cat ../a/p.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../a/p.txt\\n' \"$out\" > \"$dep\"\n"
        );
        f.write_file("c/Tupfile", ": gen.sh |> sh gen.sh %o |> d.o\n");
        f.write_file(
            "c/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../b/c.o ]; then cat ../b/c.o > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../b/c.o\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("only the head of the chain changes")
        {
            f.write_file("a/seed.txt", "v7\n");
            REQUIRE(f.build().success());

            THEN("the change reached the far end")
            {
                INFO("c.o: " << f.read_file("b/c.o"));
                INFO("d.o: " << f.read_file("c/d.o"));
                REQUIRE(f.read_file("b/c.o") == "v7\n");
                REQUIRE(f.read_file("c/d.o") == "v7\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A producer's input change reaches an output-less discovered consumer", "[e2e][incremental][implicit]")
{
    // The cascade marks the consumer command node itself, so a reader with no output path is
    // reached the same way one with outputs is; routing through outputs instead would drop it,
    // the shape #228 had on the comparison route (#284).
    GIVEN("a settled gate that declares no outputs and only discovered the generated header")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": seed.txt |> sleep 1; cat seed.txt > %o |> p.h\n");
        f.write_file("a/seed.txt", "#define VAL 1\n");
        f.write_file("b/Tupfile", "# the gate arrives once the header exists\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        // The gate's dep scan must see the header to record it; introduced together they race
        // and the discovery is silently empty.
        f.write_file("b/Tupfile", ": gate.c |> gcc -c %f -o /dev/null |>\n");
        f.write_file(
            "b/gate.c",
            "#include \"../a/p.h\"\n"
            "_Static_assert(VAL == 1, \"gate saw the old header\");\n"
            "int f(void) { return VAL; }\n"
        );
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("only the producer's input changes")
        {
            f.write_file("a/seed.txt", "#define VAL 2\n");
            auto rebuild = f.build();

            THEN("the gate was scheduled too, so the regenerated header broke its compile")
            {
                INFO("stdout: " << rebuild.stdout_output);
                INFO("stderr: " << rebuild.stderr_output);
                auto combined = rebuild.stdout_output + rebuild.stderr_output;
                REQUIRE_FALSE(rebuild.success());
                REQUIRE(combined.find("FAILED: gcc -c gate.c -o /dev/null") != std::string::npos);
                REQUIRE(combined.find("gate saw the old header") != std::string::npos);
            }
        }
    }
}

SCENARIO("A discovered consumer re-runs for a producer that rewrites the same bytes", "[e2e][incremental][implicit]")
{
    // Membership routes, not content: the consumer re-runs because its producer ran, exactly as
    // a declared consumer does. Pinned so the pessimism is a decision rather than a surprise, and
    // so that it costs one run per producer run and not one per build (#277).
    GIVEN("a settled project whose producer writes constant output from a changing input")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": seed.txt |> cat seed.txt > /dev/null; echo constant > %o |> p.txt\n");
        f.write_file("a/seed.txt", "v1\n");
        f.write_file("b/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "b/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "cat ../a/p.txt > \"$out\"\n"
            "printf '%s: ../a/p.txt\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("the producer's input changes without changing its output")
        {
            f.write_file("a/seed.txt", "v2\n");
            auto rebuild = f.build();

            THEN("both commands run")
            {
                INFO("stdout: " << rebuild.stdout_output);
                REQUIRE(rebuild.success());
                REQUIRE(rebuild.stdout_output.find("Build completed: 2 commands") != std::string::npos);
            }

            AND_WHEN("the input stops changing")
            {
                THEN("it settles rather than re-running every build")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A dependency absent when it was recorded settles rather than re-running forever", "[e2e][incremental][implicit]")
{
    // A command may report reading a file that is not there -- a conditional include that
    // resolved to nothing. Reading the same stat failure as news every build re-runs the command
    // forever for output that cannot change, which is the loop the campaign exists to kill. tup
    // records the absence as a ghost and settles, and re-runs only if the file appears (#281).
    GIVEN("a consumer whose dependency report names a file that does not exist")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": c.sh |> sh c.sh %o |> c.o\n");
        f.write_file(
            "c.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f h.txt ]; then cat h.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: h.txt\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("the project is built again")
        {
            THEN("it has settled")
            {
                auto settled = f.build();
                INFO("stdout: " << settled.stdout_output);
                REQUIRE(settled.success());
                REQUIRE(settled.is_noop());
            }
        }
    }
}

SCENARIO("A dependency that was absent when recorded still re-runs its reader when it appears", "[e2e][incremental][implicit]")
{
    // What settling must not cost: the absence is recorded as zero size, zero mtime and a zero
    // hash, and a file arriving has to be read as a change against all three (#281).
    GIVEN("a settled consumer whose dependency report names a file that does not exist")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": c.sh |> sh c.sh %o |> c.o\n");
        f.write_file(
            "c.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f h.txt ]; then cat h.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: h.txt\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        WHEN("the named file appears")
        {
            f.write_file("h.txt", "arrived\n");
            REQUIRE(f.build().success());

            THEN("the consumer ran against its contents")
            {
                INFO("c.o: " << f.read_file("c.o"));
                REQUIRE(f.read_file("c.o") == "arrived\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }

        WHEN("the named file appears empty")
        {
            // Size and mtime match the sentinel zeros, so only the recorded hash separates it.
            f.write_file("h.txt", "");
            REQUIRE(f.build().success());

            THEN("the consumer ran against it anyway")
            {
                INFO("c.o: " << f.read_file("c.o"));
                REQUIRE(f.read_file("c.o").empty());
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A deleted dependency a command still reports re-runs it once", "[e2e][incremental][implicit]")
{
    // The deletion is a real change and must reach the reader, but the run that follows records
    // the file as absent -- so the build after it has nothing new to say and must settle (#281).
    GIVEN("a settled consumer whose dependency is then deleted")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": c.sh |> sh c.sh %o |> c.o\n");
        f.write_file(
            "c.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f h.txt ]; then cat h.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: h.txt\\n' \"$out\" > \"$dep\"\n"
        );
        f.write_file("h.txt", "present\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("c.o") == "present\n");
        REQUIRE(f.build().is_noop());

        WHEN("the dependency is deleted")
        {
            f.remove_file("h.txt");
            REQUIRE(f.build().success());

            THEN("the consumer ran against its absence")
            {
                INFO("c.o: " << f.read_file("c.o"));
                REQUIRE(f.read_file("c.o") == "missing\n");
            }

            AND_WHEN("it is built once more")
            {
                THEN("it has settled")
                {
                    auto settled = f.build();
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.is_noop());
                }
            }
        }
    }
}

SCENARIO("A recreated dependency does not carry its deletion mark forward", "[e2e][incremental][implicit]")
{
    // The merge copies an out-of-scope file's entry verbatim. Run before the discovered deps,
    // that copy shadowed the fresh one, so a carried NodeFlags::AbsenceRouted discharged the next
    // real deletion as "already routed" and the consumer never ran again (#237).
    GIVEN("a guarded producer, a consumer that discovers its output, and an out-of-scope declarer")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.mkdir("d");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", "ifdef FOO\n: |> echo produced > %o |> p.txt\nendif\n");
        f.write_file("b/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "b/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../a/p.txt ]; then cat ../a/p.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../a/p.txt\\n' \"$out\" > \"$dep\"\n"
        );
        f.write_file("d/Tupfile", ": ../a/p.txt |> cp %f %o |> o.copy\n");
        f.write_file("tup.config", "CONFIG_FOO=y\n");
        // Nothing orders these two commands on a first build — b/ depends on a/p.txt only by
        // discovery — so c.o's content here is whichever won, and asserting it raced on CI.
        // gen.sh writes the .d either way, so the dependency is recorded regardless, which is
        // all the steps below need.
        REQUIRE(f.build().success());

        WHEN("the guard is turned off, the output is recreated by hand, and then deleted again")
        {
            f.write_file("tup.config", "# CONFIG_FOO off\n");
            REQUIRE(f.build().success());
            REQUIRE_FALSE(f.exists("a/p.txt"));

            f.write_file("a/p.txt", "hand-made\n");
            REQUIRE(f.build({ "b/" }).success());
            REQUIRE(f.read_file("b/c.o") == "hand-made\n");

            f.remove_file("a/p.txt");
            auto result = f.build({ "b/" });

            THEN("the consumer runs for the deletion instead of keeping a stale output")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("b/c.o") == "missing\n");
            }
        }
    }
}

SCENARIO("A build whose discovered dependency was deleted quiesces", "[e2e][incremental][implicit]")
{
    // The command re-runs and rediscovers nothing, which the carry logic could not tell from "did not run", so the dead edge and its file entry came back every build (#224).
    GIVEN("a rule whose command reports its own dependencies")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": gen.sh |> sh gen.sh %o |> out.o\n");
        f.write_file(
            "gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f extra.txt ]; then\n"
            "  cat extra.txt > \"$out\"\n"
            "  printf '%s: extra.txt\\n' \"$out\" > \"$dep\"\n"
            "else\n"
            "  echo base > \"$out\"\n"
            "  : > \"$dep\"\n"
            "fi\n"
        );
        f.write_file("extra.txt", "from-extra\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("the discovered dependency is deleted")
        {
            f.remove_file("extra.txt");
            auto heal = f.build();
            REQUIRE(heal.success());
            REQUIRE_FALSE(heal.is_noop());

            THEN("the build after the healing one does nothing")
            {
                auto result = f.build();
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.is_noop());
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

SCENARIO("A header the compile reads only under -O2 is tracked", "[e2e][incremental]")
{
    // A scan without the compile's flags resolves the other branch and records the wrong header.
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("a source whose include is gated on __OPTIMIZE__")
    {
        auto f = E2EFixture { "optimize_gated_header" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("program").stdout_output == "1\n");
        REQUIRE(f.build().is_noop());

        WHEN("the header the compile actually read changes")
        {
            f.write_file("opt.h", "#ifndef OPT_H\n"
                                  "#define OPT_H\n"
                                  "#define VALUE 2\n"
                                  "#endif\n");
            auto result = f.build();

            THEN("the compile reruns")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }

            THEN("the program reflects the change")
            {
                REQUIRE(result.success());
                REQUIRE(f.run("program").stdout_output == "2\n");
            }
        }
    }
}

SCENARIO("A dep scan that prints nothing fails the build", "[e2e][incremental]")
{
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("a compiler whose scan writes its rule somewhere else")
    {
        auto f = E2EFixture { "scan_output_empty" };
        REQUIRE(f.init().success());

        WHEN("the build runs")
        {
            auto result = f.build();

            THEN("it fails")
            {
                REQUIRE_FALSE(result.success());
            }

            THEN("it names the scan that discovered nothing")
            {
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("./tools/gcc -M f.c") != std::string::npos);
            }
        }
    }
}

SCENARIO("A dep scan that prints anything but its rule fails the build", "[e2e][incremental]")
{
    // Chatter parsed as dependencies never stats, so the command would re-run for ever.
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("a compiler whose scan prints a note before its rule")
    {
        auto f = E2EFixture { "scan_output_garbage" };
        REQUIRE(f.init().success());

        WHEN("the build runs")
        {
            auto result = f.build();

            THEN("it fails")
            {
                REQUIRE_FALSE(result.success());
            }

            THEN("it names the scan that misbehaved")
            {
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("./tools/gcc -M f.c") != std::string::npos);
            }
        }
    }
}

SCENARIO("Implicit deps survive a flag whose path is a separate word", "[e2e][incremental]")
{
    // A flag's path reaches the scan whichever spelling carries it, or the scan has no input file.
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("a compile whose sysroot is spelled as two words")
    {
        auto f = E2EFixture { "sysroot_separate_word" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.run("program").stdout_output == "1\n");
        REQUIRE(f.build().is_noop());

        WHEN("the header it includes changes")
        {
            f.write_file("value.h", "#ifndef VALUE_H\n"
                                    "#define VALUE_H\n"
                                    "#define VALUE 2\n"
                                    "#endif\n");
            auto result = f.build();

            THEN("the compile reruns")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }

            THEN("the program reflects the change")
            {
                REQUIRE(result.success());
                REQUIRE(f.run("program").stdout_output == "2\n");
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

SCENARIO("A build run from a subdirectory does not stamp an out-of-scope change as current", "[e2e][incremental][scope]")
{
    // A cwd-derived scope parses the whole project but detects only its own directory, while the
    // record leg re-hashed every graph file: a/src.txt was recorded current on the strength of a
    // stat nothing consumed, so the next full build compared v2 against v2 forever (#288).
    GIVEN("two independent directories, settled")
    {
        auto f = E2EFixture { "scoped_out_of_scope_edit" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("a/out.txt") == "v1\n");

        WHEN("the out-of-scope source changes and a build runs from the other directory")
        {
            f.write_file("a/src.txt", "v2\n");
            f.write_file("b/src.txt", "v2\n");
            auto scoped = f.run_pup_in_dir("b", {});
            REQUIRE(scoped.success());
            REQUIRE_FALSE(scoped.is_noop());

            THEN("the next full build still rebuilds the directory it never examined")
            {
                auto full = f.build();
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.success());
                REQUIRE(f.read_file("a/out.txt") == "v2\n");
            }
        }
    }
}

SCENARIO("A build with --all-deps does not stamp an out-of-scope change as current", "[e2e][incremental][scope]")
{
    // -a empties parse_scopes for the same reason cwd scoping does, so it reaches #288 by the
    // same door: everything is parsed, only the scope is detected, everything is recorded.
    GIVEN("two independent directories, settled")
    {
        auto f = E2EFixture { "scoped_out_of_scope_edit" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("a/out.txt") == "v1\n");

        WHEN("the out-of-scope source changes and a scoped -a build runs")
        {
            f.write_file("a/src.txt", "v2\n");
            f.write_file("b/src.txt", "v2\n");
            auto scoped = f.build({ "-a", "b/" });
            REQUIRE(scoped.success());
            REQUIRE_FALSE(scoped.is_noop());

            THEN("the next full build still rebuilds the directory it never examined")
            {
                auto full = f.build();
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.success());
                REQUIRE(f.read_file("a/out.txt") == "v2\n");
            }
        }
    }
}

SCENARIO("A file added while building from a subdirectory is still built", "[e2e][incremental][scope]")
{
    // The other side of #288's fix: carrying an unexamined file's recorded state forward must not
    // become "record nothing", or a file first seen by a scoped build would never be built.
    GIVEN("a settled project whose other directory globs its sources")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": foreach *.txt |> cp %f %o |> %B.out\n");
        f.write_file("a/one.txt", "1\n");
        f.write_file("b/Tupfile", ": in.txt |> cat %f > %o |> b.out\n");
        f.write_file("b/in.txt", "i1\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.build().is_noop());

        WHEN("a file appears in the other directory and a build runs from this one")
        {
            f.write_file("a/two.txt", "2\n");
            f.write_file("b/in.txt", "i2\n");
            REQUIRE(f.run_pup_in_dir("b", {}).success());

            THEN("the added file is built no later than the next full build")
            {
                auto full = f.build();
                INFO("stdout: " << full.stdout_output);
                REQUIRE(full.success());
                REQUIRE(f.read_file("a/two.out") == "2\n");
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

SCENARIO("A scoped build keeps the record of what an out-of-scope command produced", "[e2e][incremental][scoped-config]")
{
    GIVEN("an out-of-scope command whose input a scoped build is about to rebuild")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/src.txt", "v1\n");
        f.write_file("a/Tupfile", ": src.txt |> cp %f %o |> gen.txt\n");
        f.write_file("b/Tupfile", ": ../a/gen.txt |> cp %f %o |> out.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("b/out.txt"));

        WHEN("the scoped build rebuilds that input and the out-of-scope rule is then dropped")
        {
            f.write_file("a/src.txt", "v2\n");
            REQUIRE(f.build({ "a/" }).success());
            f.write_file("b/Tupfile", "# no rules\n");
            auto result = f.build({ "-v" });

            THEN("the output it produced is still known, and is deleted")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("b/out.txt"));
            }
        }
    }
}

SCENARIO("An out-of-scope command is marked when its order-only input changes", "[e2e][incremental][order-only]")
{
    GIVEN("an out-of-scope command reaching a rebuilt file only through an order-only input")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/src.txt", "v1\n");
        f.write_file("a/Tupfile", ": src.txt |> cp %f %o |> gen.txt\n");
        f.write_file("b/base.txt", "base\n");
        f.write_file("b/Tupfile", ": base.txt | ../a/gen.txt |> cat base.txt ../a/gen.txt > %o |> out.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("b/out.txt") == "base\nv1\n");

        WHEN("a scoped build rebuilds that file while the consumer is out of scope")
        {
            f.write_file("a/src.txt", "v2\n");
            REQUIRE(f.build({ "a/" }).success());

            THEN("the next full build re-runs the consumer")
            {
                auto result = f.build({ "-v" });
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("b/out.txt") == "base\nv2\n");
            }
        }
    }
}

SCENARIO("A config rule a build will not run does not stay marked unverified", "[e2e][incremental][configure]")
{
    GIVEN("a config rule whose input a scoped build is about to change")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.write_file("Tupfile", ": configs/board.config |> cp %f %o |> tup.config\n: tup.config |> cp %f %o |> conf_copy.txt\n");
        f.write_file("configs/Tupfile", ": board.config |> cp %f %o |> copy.txt\n");
        f.write_file("sub/Tupfile", "# no rules\n");
        f.write_file("configs/board.config", "CONFIG_MSG=one\n");
        REQUIRE(f.pup({ "configure" }).success());
        REQUIRE(f.build().success());

        WHEN("the scoped build changes it while the config rule is out of scope")
        {
            f.write_file("configs/board.config", "CONFIG_MSG=two\n");
            REQUIRE(f.build({ "configs/" }).success());
            REQUIRE(f.build().success());

            THEN("the build after that has nothing left to do")
            {
                auto settled = f.build();
                INFO("stdout: " << settled.stdout_output);
                REQUIRE(settled.success());
                REQUIRE(settled.is_noop());
            }
        }
    }
}

SCENARIO("An unverified record survives a build that scheduled it without running it", "[e2e][incremental][scoped-config]")
{
    GIVEN("an out-of-scope command whose record a scoped build has marked unverified")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/src.txt", "v1\n");
        f.write_file("a/Tupfile", ": src.txt |> cp %f %o |> gen.txt\n");
        f.write_file("b/Tupfile", ": ../a/gen.txt |> cp %f %o |> out.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        f.write_file("a/src.txt", "v2\n");
        REQUIRE(f.build({ "a/" }).success());

        WHEN("a target build schedules it but the target filter keeps it from running")
        {
            REQUIRE(f.build({ "a/gen.txt" }).success());

            THEN("the next full build still runs it")
            {
                auto result = f.build({ "-v" });
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("b/out.txt") == "v2\n");
            }
        }
    }
}

SCENARIO("A directory that failed to parse keeps the record of what it produced", "[e2e][incremental][keep-going]")
{
    GIVEN("a project in which one directory has stopped parsing")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/in.txt", "x\n");
        f.write_file("b/in.txt", "y\n");
        f.write_file("a/Tupfile", ": foreach *.txt |> cp %f %o |> %B.o\n");
        f.write_file("b/Tupfile", ": foreach *.txt |> cp %f %o |> %B.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("b/in.o"));

        WHEN("a keep-going build runs past the parse failure and the rule is then dropped")
        {
            f.write_file("b/Tupfile", "include nope.tup\n");
            REQUIRE_FALSE(f.build({ "-k" }).success());
            f.write_file("b/Tupfile", "# no rules\n");
            auto result = f.build({ "-v" });

            THEN("the output it produced is still known, and is deleted")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("b/in.o"));
            }
        }
    }
}

SCENARIO("A project whose last rule is removed deletes the output it built", "[e2e][incremental][stale]")
{
    GIVEN("a project whose single rule has produced its output")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("in.txt", "hello\n");
        f.write_file("Tupfile", ": foreach *.txt |> cp %f %o |> %B.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("in.o"));

        WHEN("that rule is removed, leaving the project with none")
        {
            f.write_file("Tupfile", "# no rules\n");
            auto result = f.build({ "-v" });

            THEN("the output it produced is deleted, and the deletion is reported")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("in.o"));
                REQUIRE(result.stdout_output.find("Removed stale: in.o") != std::string::npos);
            }

            AND_WHEN("the project is built again")
            {
                auto settled = f.build({ "-v" });

                THEN("the retired command is not reported a second time")
                {
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.stdout_output.find("Removed command") == std::string::npos);
                }
            }
        }
    }
}

SCENARIO("A stale output that cannot be deleted fails the build and keeps its record", "[e2e][incremental][stale]")
{
    GIVEN("a rule that has been removed, whose output is in a directory that cannot be written")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": |> echo x > %o |> a.o\n");
        f.write_file("b/Tupfile", ": |> echo y > %o |> b.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("a/a.o"));
        f.write_file("a/Tupfile", "# no rules\n");

        auto const dir = f.workdir() / "a";
        auto const writable = std::filesystem::status(dir).permissions();
        std::filesystem::permissions(
            dir,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace
        );
        auto probe_ec = std::error_code {};
        std::filesystem::create_directory(dir / "probe", probe_ec);
        if (!probe_ec) {
            std::filesystem::remove(dir / "probe", probe_ec);
            std::filesystem::permissions(dir, writable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke write permission (running as root?): scenario not exercised");
            return;
        }

        WHEN("the build tries to delete that stale output")
        {
            auto result = f.build({ "-v" });
            std::filesystem::permissions(dir, writable, std::filesystem::perm_options::replace);

            THEN("it fails, naming the file it could not remove")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE((result.stdout_output + result.stderr_output).find("a/a.o") != std::string::npos);
            }

            AND_WHEN("the directory becomes writable and the project is built again")
            {
                auto settled = f.build({ "-v" });

                THEN("the record that survived deletes the stale output after all")
                {
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE_FALSE(f.exists("a/a.o"));
                    REQUIRE(settled.stdout_output.find("Removed stale: a/a.o") != std::string::npos);
                }
            }
        }
    }
}

SCENARIO("A clean dry run counts the directories it lists", "[e2e][clean][dry-run]")
{
    GIVEN("a build whose only generated directory would be removed")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> echo x > %o |> out/gen.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("out/gen.o"));

        WHEN("clean runs with -n")
        {
            auto result = f.clean({ "-n", "-v" });

            THEN("the summary counts the directory it just said it would remove")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("out/gen.o"));
                REQUIRE(result.stdout_output.find("Would remove empty dir") != std::string::npos);
                REQUIRE(result.stdout_output.find("Would remove 1 files, 1 directories") != std::string::npos);
            }
        }
    }
}

SCENARIO("clean does not count an empty directory it could not remove", "[e2e][clean]")
{
    GIVEN("a build whose only generated directory sits in a root that cannot be written")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> echo x > %o |> out/gen.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("out/gen.o"));

        auto const root = f.workdir();
        auto const writable = std::filesystem::status(root).permissions();
        std::filesystem::permissions(
            root,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace
        );
        auto probe_ec = std::error_code {};
        std::filesystem::create_directory(root / "probe", probe_ec);
        if (!probe_ec) {
            std::filesystem::remove(root / "probe", probe_ec);
            std::filesystem::permissions(root, writable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke write permission (running as root?): scenario not exercised");
            return;
        }

        WHEN("clean removes the file but cannot remove the directory")
        {
            auto result = f.clean({ "-v" });
            std::filesystem::permissions(root, writable, std::filesystem::perm_options::replace);

            THEN("it neither counts nor announces the directory as removed")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(f.is_directory("out"));
                REQUIRE(combined.find("Removed empty dir") == std::string::npos);
                REQUIRE(combined.find("Failed to remove directory") != std::string::npos);
                REQUIRE(combined.find("0 directories") != std::string::npos);
                REQUIRE_FALSE(result.success());
                // The platform message already names the path; the caller must not repeat it.
                auto const dir_name = std::string { "/out:" };
                auto first = combined.find(dir_name);
                REQUIRE(first != std::string::npos);
                REQUIRE(combined.find(dir_name, first + 1) == std::string::npos);
            }
        }
    }
}

SCENARIO("distclean does not report a reset it could not perform", "[e2e][clean]")
{
    GIVEN("a configured project whose build directory cannot be written")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> echo x > %o |> out/gen.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        auto const root = f.workdir();
        auto const writable = std::filesystem::status(root).permissions();
        std::filesystem::permissions(
            root,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace
        );
        auto probe_ec = std::error_code {};
        std::filesystem::create_directory(root / "probe", probe_ec);
        if (!probe_ec) {
            std::filesystem::remove(root / "probe", probe_ec);
            std::filesystem::permissions(root, writable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke write permission (running as root?): scenario not exercised");
            return;
        }

        WHEN("distclean can remove neither the index directory nor tup.config")
        {
            auto result = f.distclean({ "-v" });
            std::filesystem::permissions(root, writable, std::filesystem::perm_options::replace);

            THEN("it fails instead of claiming the project was reset")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(f.exists("tup.config"));
                REQUIRE(f.is_directory(".pup"));
                REQUIRE(combined.find("Project reset complete") == std::string::npos);
                REQUIRE_FALSE(result.success());
            }
        }
    }
}

SCENARIO("clean leaves a source file an inactive branch merely declares", "[e2e][clean][conditionals]")
{
    GIVEN("a committed source whose path an unbuilt conditional branch names as an output")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", "ifeq (@(FOO),y)\n: |> cp src.txt %o |> foo.txt\nendif\n");
        f.write_file("src.txt", "hello\n");
        f.write_file("foo.txt", "I AM A COMMITTED SOURCE FILE\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().is_noop());

        WHEN("clean removes what the record says the build owns")
        {
            auto result = f.clean({ "-v" });

            THEN("the source file is still there")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(f.exists("foo.txt"));
                REQUIRE(f.read_file("foo.txt") == "I AM A COMMITTED SOURCE FILE\n");
                REQUIRE(result.stdout_output.find("Removed: foo.txt") == std::string::npos);
            }
        }
    }
}

SCENARIO("A source and the out-of-tree output shadowing it are one record clean can read", "[e2e][clean][out-of-tree]")
{
    // The regression pin for comparing recorded paths as stored: this record holds one file as
    // both a source and an output, and only their spellings tell them apart (#382).
    GIVEN("a build whose output shadows a committed source of the same name")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("z");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": bar.txt |> cat %f > %o |> a.out\n");
        f.write_file("z/Tupfile", ": |> echo GEN > %o |> ../a/bar.txt\n");
        f.write_file("a/bar.txt", "COMMITTED SOURCE\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());

        WHEN("clean removes what the record says the build produced")
        {
            auto result = f.clean({ "-B", "build", "-v" });

            THEN("it succeeds, removes the output, and leaves the source")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("a/bar.txt") == "COMMITTED SOURCE\n");
                REQUIRE_FALSE(f.exists("build/a/bar.txt"));
            }
        }
    }
}

SCENARIO("A build refuses to overwrite a file the record does not attribute to a rule", "[e2e][build][conditionals]")
{
    // Whether a previous build happened must not change the answer: the record is what the
    // guard reads, and none of these three sequences gives it a claim on the file.
    GIVEN("a conditional branch whose output path holds a file no rule has produced")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", "ifeq (@(FOO),y)\n: |> cp src.txt %o |> foo.txt\nendif\n");
        f.write_file("src.txt", "hello\n");
        REQUIRE(f.init().success());

        WHEN("the branch is turned on after a build that ran with it off")
        {
            f.write_file("foo.txt", "I AM A COMMITTED SOURCE FILE\n");
            REQUIRE(f.build().is_noop());
            auto result = f.build({ "-D", "FOO=y" });

            THEN("the build refuses, names the file, and leaves it untouched")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(f.read_file("foo.txt") == "I AM A COMMITTED SOURCE FILE\n");
                REQUIRE(combined.find("does not own") != std::string::npos);
                REQUIRE(combined.find("foo.txt") != std::string::npos);
                REQUIRE_FALSE(result.success());
            }
        }

        WHEN("the branch is turned on with no previous build")
        {
            f.write_file("foo.txt", "I AM A COMMITTED SOURCE FILE\n");
            auto result = f.build({ "-D", "FOO=y" });

            THEN("the build refuses, names the file, and leaves it untouched")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(f.read_file("foo.txt") == "I AM A COMMITTED SOURCE FILE\n");
                REQUIRE(combined.find("does not own") != std::string::npos);
                REQUIRE(combined.find("foo.txt") != std::string::npos);
                REQUIRE_FALSE(result.success());
            }
        }

        WHEN("the file is created after a build that recorded the path as absent")
        {
            REQUIRE(f.build().is_noop());
            f.write_file("foo.txt", "HAND MADE, PRECIOUS\n");
            auto result = f.build({ "-D", "FOO=y" });

            THEN("the build refuses, names the file, and leaves it untouched")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(f.read_file("foo.txt") == "HAND MADE, PRECIOUS\n");
                REQUIRE(combined.find("does not own") != std::string::npos);
                REQUIRE(combined.find("foo.txt") != std::string::npos);
                REQUIRE_FALSE(result.success());
            }
        }
    }
}

SCENARIO("Editing a source file an inactive branch names re-runs its consumer", "[e2e][incremental][conditionals]")
{
    GIVEN("a committed source that an unbuilt branch declares and an active rule consumes")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file(
            "Tupfile",
            "ifeq (@(FOO),y)\n: |> cp src.txt %o |> foo.txt\nendif\n"
            ": foo.txt |> cat %f > %o |> out.txt\n"
        );
        f.write_file("src.txt", "hello\n");
        f.write_file("foo.txt", "first\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("out.txt") == "first\n");

        WHEN("the source file is edited")
        {
            f.write_file("foo.txt", "second\n");
            auto result = f.build();

            THEN("the rule that consumes it runs again")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("out.txt") == "second\n");
            }
        }
    }
}

SCENARIO("A file appearing where only an inactive branch declared it re-runs its discoverer", "[e2e][incremental][implicit][conditionals]")
{
    GIVEN("a path an inactive branch declares and an active command discovers")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", "ifdef FOO\n: |> echo produced > %o |> p.txt\nendif\n");
        f.write_file("b/Tupfile", ": gen.sh |> sh gen.sh %o |> c.o\n");
        f.write_file(
            "b/gen.sh",
            "out=$1\n"
            "dep=\"${out%.o}.d\"\n"
            "if [ -f ../a/p.txt ]; then cat ../a/p.txt > \"$out\"; else echo missing > \"$out\"; fi\n"
            "printf '%s: ../a/p.txt\\n' \"$out\" > \"$dep\"\n"
        );
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("b/c.o") == "missing\n");

        WHEN("the file is created by hand")
        {
            f.write_file("a/p.txt", "hand-made\n");
            auto result = f.build();

            THEN("the command that discovered the path runs again")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("b/c.o") == "hand-made\n");
            }
        }
    }
}

SCENARIO("A glob skips a path only an inactive branch declares", "[e2e][build][glob][conditionals]")
{
    GIVEN("a glob rule beside a conditional branch that is not taken")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file(
            "Tupfile",
            "ifeq (@(FOO),y)\n: |> echo x > %o |> bar.txt\nendif\n"
            ": foreach *.txt |> cat %f > %o |> %B.out\n"
        );
        f.write_file("src.txt", "hello\n");

        WHEN("the project is built")
        {
            REQUIRE(f.init().success());
            auto result = f.build();

            THEN("only the file that exists is expanded")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.exists("src.out"));
                REQUIRE_FALSE(f.exists("bar.out"));
                REQUIRE(result.success());
            }
        }
    }
}

SCENARIO("A subdirectory an inactive branch would write into is not generated", "[e2e][build][conditionals]")
{
    GIVEN("a committed source in a subdirectory an unbuilt branch names as an output")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("sub");
        f.write_file("Tupfile", "ifeq (@(FOO),y)\n: |> cp src.txt %o |> sub/foo.txt\nendif\n");
        f.write_file("src.txt", "hello\n");
        f.write_file("sub/foo.txt", "I AM A COMMITTED SOURCE FILE\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().is_noop());

        WHEN("clean removes what the record says the build owns")
        {
            auto result = f.clean({ "-v" });

            THEN("neither the source nor its directory is removed")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(f.exists("sub/foo.txt"));
                REQUIRE(f.read_file("sub/foo.txt") == "I AM A COMMITTED SOURCE FILE\n");
                REQUIRE(f.is_directory("sub"));
            }
        }
    }
}

SCENARIO("Turning a branch off keeps ownership of what it built", "[e2e][build][conditionals]")
{
    // Ownership survives the branch going inactive only via the record's carry-forward (#369).
    GIVEN("an output produced while its branch was active")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", "ifeq (@(FOO),y)\n: |> cp src.txt %o |> foo.txt\nendif\n");
        f.write_file("src.txt", "hello\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build({ "-D", "FOO=y" }).success());
        REQUIRE(f.exists("foo.txt"));

        WHEN("the branch is turned off again")
        {
            auto result = f.build({ "-v" });

            THEN("the build deletes the output it owned rather than abandoning it")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("foo.txt"));
                REQUIRE(result.stdout_output.find("Removed stale: foo.txt") != std::string::npos);
            }
        }
    }
}

SCENARIO("A stale output that cannot even be queried keeps its record", "[e2e][incremental][stale]")
{
    // The rule lives in the readable root Tupfile so its directory stays authoritative;
    // only the output's directory is locked, which is what reaches the exists() guard.
    GIVEN("a removed rule whose output sits in a directory that cannot be read")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.write_file("Tupfile", ": |> echo x > %o |> a/gen.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("a/gen.o"));
        f.write_file("Tupfile", "# no rules\n");

        auto const dir = f.workdir() / "a";
        auto const readable = std::filesystem::status(dir).permissions();
        std::filesystem::permissions(dir, std::filesystem::perms::none, std::filesystem::perm_options::replace);
        auto probe_ec = std::error_code {};
        std::filesystem::create_directory(dir / "probe", probe_ec);
        if (!probe_ec) {
            std::filesystem::remove(dir / "probe", probe_ec);
            std::filesystem::permissions(dir, readable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke directory permissions (running as root?): scenario not exercised");
            return;
        }

        WHEN("the build cannot determine whether the stale output is there")
        {
            auto locked = f.build({ "-v" });
            std::filesystem::permissions(dir, readable, std::filesystem::perm_options::replace);

            THEN("it fails rather than silently dropping the record")
            {
                INFO("stdout: " << locked.stdout_output);
                INFO("stderr: " << locked.stderr_output);
                REQUIRE_FALSE(locked.success());
            }

            AND_WHEN("the directory becomes readable and the project is built again")
            {
                auto healed = f.build({ "-v" });

                THEN("the surviving record deletes the stale output")
                {
                    INFO("stdout: " << healed.stdout_output);
                    REQUIRE(healed.success());
                    REQUIRE_FALSE(f.exists("a/gen.o"));
                }
            }
        }
    }
}

SCENARIO("A rule turned off by a guard is reported removed only once", "[e2e][incremental][stale]")
{
    GIVEN("a guarded rule whose output has been built")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("tup.config", "CONFIG_FOO=y\n");
        f.write_file("in.txt", "hello\n");
        f.write_file("Tupfile", "ifdef FOO\n: in.txt |> cp %f %o |> in.o\nendif\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("in.o"));

        WHEN("the guard is turned off")
        {
            f.write_file("tup.config", "# FOO off\n");
            auto result = f.build({ "-v" });

            THEN("the output it built is deleted and the removal is reported")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("in.o"));
                REQUIRE(result.stdout_output.find("Removed command") != std::string::npos);
            }

            AND_WHEN("the project is built again")
            {
                auto settled = f.build({ "-v" });

                THEN("the retired command is not reported a second time")
                {
                    INFO("stdout: " << settled.stdout_output);
                    REQUIRE(settled.success());
                    REQUIRE(settled.stdout_output.find("Removed command") == std::string::npos);
                }
            }
        }
    }
}

SCENARIO("A dry run reports the command removal it would make, not one it made", "[e2e][incremental][stale]")
{
    GIVEN("a rule that has been removed, whose output is still on disk")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": |> echo x > %o |> a.o\n");
        f.write_file("b/Tupfile", ": |> echo y > %o |> b.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("a/a.o"));
        f.write_file("a/Tupfile", "# no rules\n");

        WHEN("the project is built with -n")
        {
            auto result = f.build({ "-n", "-v" });

            THEN("both the output and the command are reported as removals it would make")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("a/a.o"));
                REQUIRE(result.stdout_output.find("Would remove stale: a/a.o") != std::string::npos);
                REQUIRE(result.stdout_output.find("Would remove command") != std::string::npos);
                REQUIRE(result.stdout_output.find("Removed command") == std::string::npos);
            }
        }
    }
}

SCENARIO("A dry run does not initialize the project", "[e2e][build][dry-run]")
{
    GIVEN("a project that has never been built")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("in.txt", "hello\n");
        f.write_file("Tupfile", ": in.txt |> cp %f %o |> out.o\n");
        f.write_file("tup.config", "");
        REQUIRE_FALSE(f.exists(".pup"));

        WHEN("it is built with -n")
        {
            auto result = f.build({ "-n" });

            THEN("no .pup directory is created and the report is future tense")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(f.exists(".pup"));
                REQUIRE(result.stdout_output.find("Initialized pup") == std::string::npos);
            }
        }
    }
}

SCENARIO("A dry run summarises what it would run, not a build it completed", "[e2e][build][dry-run]")
{
    GIVEN("a project with one command that has never been built")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("in.txt", "hello\n");
        f.write_file("Tupfile", ": in.txt |> cp %f %o |> out.o\n");
        REQUIRE(f.init().success());

        WHEN("the project is built with -n")
        {
            auto result = f.build({ "-n" });

            THEN("the summary is future tense and no output was produced")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("out.o"));
                REQUIRE(result.stdout_output.find("Would run: 1 commands") != std::string::npos);
                REQUIRE(result.stdout_output.find("Build completed") == std::string::npos);
            }
        }

        WHEN("a real build runs")
        {
            auto result = f.build();

            THEN("the summary still reports the build it completed")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("out.o"));
                REQUIRE(result.stdout_output.find("Build completed: 1 commands") != std::string::npos);
            }
        }
    }
}

SCENARIO("A configure dry run neither claims nor performs the work", "[e2e][configure][dry-run]")
{
    GIVEN("a project whose tup.config a config rule would generate")
    {
        auto f = E2EFixture { "configure_cmd" };
        f.write_file("Tupfile", ": configs/board.config |> cp %f %o |> tup.config\n");
        f.write_file("configs/Tupfile", "# no rules\n");
        f.write_file("sub/Tupfile", "# no rules\n");
        f.write_file("configs/board.config", "CONFIG_MSG=one\n");

        WHEN("configure runs with -n")
        {
            auto result = f.pup({ "configure", "-n" });

            THEN("it reports future work and writes no tup.config")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("tup.config"));
                REQUIRE(result.stdout_output.find("Would run: 1 commands") != std::string::npos);
                REQUIRE(result.stdout_output.find("Configure completed") == std::string::npos);
                REQUIRE(result.stdout_output.find("Created ") == std::string::npos);
            }
        }

        WHEN("configure runs for real")
        {
            auto result = f.pup({ "configure" });

            THEN("it reports the work it did and the config exists")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("tup.config"));
                REQUIRE(result.stdout_output.find("Configure completed: 1 commands") != std::string::npos);
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
            // A comment would not do: an edit leaving every command's text identical is correctly a no-op (#225), so the mutation has to change a command.
            f.write_file("app/Tupfile", ": main.c ../lib/foo.o |> gcc %f -o %o -DEXTRA=1 |> app\n");
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

SCENARIO("A file that cannot be hashed is named in a warning", "[e2e][incremental]")
{
    // The only signal a user gets when content hashing fails; it had no test at all (#204).
    GIVEN("a source file that cannot be read")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("in.txt", "hello\n");
        // The command must not read in.txt, or it fails before putup ever hashes it.
        f.write_file("Tupfile", ": in.txt |> echo done > %o |> out.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());

        auto const victim = f.workdir() / "in.txt";
        auto const readable = std::filesystem::status(victim).permissions();
        std::filesystem::permissions(victim, std::filesystem::perms::none, std::filesystem::perm_options::replace);
        auto probe = std::ifstream { victim };
        if (probe.good()) {
            std::filesystem::permissions(victim, readable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke read permission (running as root?): scenario not exercised");
            return;
        }
        probe.close();

        WHEN("a build has to hash it")
        {
            f.write_file("Tupfile", ": in.txt |> echo done > %o |> renamed.o\n");
            auto result = f.build();
            std::filesystem::permissions(victim, readable, std::filesystem::perm_options::replace);

            THEN("the warning names the file it could not hash")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("Failed to hash file") != std::string::npos);
                REQUIRE(combined.find("in.txt") != std::string::npos);
            }
        }
    }
}

SCENARIO("The ghost hint offers -a only where -a could help", "[e2e][scope]")
{
    GIVEN("a consumer whose producer's rule has been deleted")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("Tupfile", "# no rules at the root\n");
        f.write_file("a/Tupfile", ": |> echo x > %o |> gen.txt\n");
        f.write_file("b/Tupfile", ": ../a/gen.txt |> cp %f %o |> out.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        f.write_file("a/Tupfile", "# no rules\n");
        f.remove_file("a/gen.txt");

        WHEN("a full build hits the missing input")
        {
            auto result = f.build();

            THEN("it names the likely cause instead of a remedy that cannot apply")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("unresolved ghost") != std::string::npos);
                REQUIRE(combined.find("building with -a") == std::string::npos);
            }
        }

        WHEN("a scoped build that already passed -a hits it")
        {
            auto result = f.build({ "b/", "-a" });

            THEN("it does not repeat the remedy the user just used")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("building with -a") == std::string::npos);
            }
        }

        WHEN("a scoped build without -a hits it")
        {
            auto result = f.build({ "b/" });

            THEN("the -a hint is still offered, because there it can help")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("building with -a") != std::string::npos);
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

            THEN("reports nothing to clean, and succeeds: no record and no build is not a failure")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
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

SCENARIO("Show graph --all-deps says so when the record cannot be read", "[e2e][show]")
{
    GIVEN("a built project whose record is then overwritten with something that is not a record")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };
        auto f = E2EFixture { "implicit_deps" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        f.write_file(".pup/index", "not an index\n");

        WHEN("show graph --all-deps is run")
        {
            auto result = f.pup({ "show", "graph", "--all-deps" });

            THEN("it fails and names the record instead of drawing a graph missing the deps it was asked for")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("build record") != std::string::npos);
                REQUIRE(result.stdout_output.find("digraph G {") == std::string::npos);
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

SCENARIO("A multi-variant run fails when one variant's command fails", "[e2e][multi-variant]")
{
    GIVEN("two variants whose shared rule fails only under DEBUG")
    {
        auto f = E2EFixture { "multi_variant" };

        f.mkdir("build-debug");
        f.mkdir("build-release");
        f.write_file("build-debug/tup.config", "CONFIG_DEBUG=y\n");
        f.write_file("build-release/tup.config", "");
        f.write_file("Tupfile",
            "ifeq (@(DEBUG),y)\n"
            ": |> sh -c \"exit 1\" > %o |> out.txt\n"
            "else\n"
            ": |> echo ok > %o |> out.txt\n"
            "endif\n");

        REQUIRE(f.pup({ "configure", "build-*" }).success());

        WHEN("both variants are built in one run")
        {
            auto result = f.build({ "-B", "build-debug", "-B", "build-release" });

            THEN("the run fails, even though the other variant succeeded")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(f.read_file("build-release/out.txt") == "ok\n");
            }
        }
    }
}

SCENARIO("A command killed by a signal fails the build", "[e2e][build]")
{
    GIVEN("a rule whose command kills itself with a signal")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("selfkill.sh", "kill -9 $$\n");
        f.write_file("Tupfile", ": |> exec sh ./selfkill.sh |>\n");
        REQUIRE(f.init().success());

        WHEN("the build runs")
        {
            auto result = f.build();

            THEN("the build fails rather than counting the signalled command as done")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("FAILED: exec sh ./selfkill.sh") != std::string::npos);
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

SCENARIO("A configure that cannot write tup.config does not report creating it", "[e2e][configure]")
{
    GIVEN("a project whose build-directory path is occupied by a regular file")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());
        // A file where the directory must go beats the exists() guard on tup.config itself,
        // and needs no permission bits, so the scenario runs under root too.
        f.write_file("blocked", "");

        WHEN("configure is run against it")
        {
            auto result = f.pup({ "configure", "-B", "blocked" });

            THEN("it fails and claims no file it did not create")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stdout_output.find("Created") == std::string::npos);
                REQUIRE(result.stderr_output.find("tup.config") != std::string::npos);
            }
        }
    }
}

SCENARIO("A configure that cannot install the config it was given does not report installing it", "[e2e][configure]")
{
    GIVEN("a config file to install and a destination that cannot be written")
    {
        auto f = E2EFixture { "simple_c" };
        REQUIRE(f.init().success());
        f.write_file("my.config", "CONFIG_FOO=bar\n");
        f.mkdir("blocked/tup.config");

        WHEN("configure is asked to install it there")
        {
            auto result = f.pup({ "configure", "--config", "my.config", "-B", "blocked" });

            THEN("it fails and claims no install it did not perform")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stdout_output.find("Installed") == std::string::npos);
                REQUIRE(f.is_directory("blocked/tup.config"));
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

SCENARIO("A build that aborted on a failure does not report the tree up to date", "[e2e][incremental][failure]")
{
    GIVEN("a settled project whose command then starts failing for a reason outside its inputs")
    {
        auto f = E2EFixture { "failed_command" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o && test ! -f gate |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        f.write_file("gate", "");

        WHEN("the command re-runs, fails, and the build aborts without keep-going")
        {
            REQUIRE_FALSE(f.build({ "--rerun" }).success());

            THEN("the next build still knows the command has not succeeded since")
            {
                auto again = f.build();
                INFO("stdout: " << again.stdout_output);
                INFO("stderr: " << again.stderr_output);
                REQUIRE(again.stdout_output.find("Nothing to do") == std::string::npos);
                REQUIRE_FALSE(again.success());
            }
        }
    }
}

SCENARIO("A build aborted before a command could run does not record it as done", "[e2e][incremental][failure]")
{
    GIVEN("eight independent rules, settled, one of which then starts failing")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        auto tupfile = std::string {};
        for (auto i = 1; i <= 8; ++i) {
            auto n = std::to_string(i);
            tupfile += ": s";
            tupfile += n;
            tupfile += ".txt |> cp %f %o";
            if (i == 2) {
                tupfile += " && test ! -f gate";
            }
            tupfile += " |> o";
            tupfile += n;
            tupfile += ".txt\n";
            f.write_file("s" + n + ".txt", "v1\n");
        }
        f.write_file("Tupfile", tupfile);
        REQUIRE(f.build().success());

        WHEN("every input changes and the build aborts serially on the failing rule")
        {
            for (auto i = 1; i <= 8; ++i) {
                f.write_file("s" + std::to_string(i) + ".txt", "v2\n");
            }
            f.write_file("gate", "");
            REQUIRE_FALSE(f.build({ "-j1" }).success());

            // Self-validating: the point of the scenario is that an abort leaves commands
            // it meant to run un-run, so it must witness one before asserting what follows.
            auto stranded = 0;
            for (auto i = 1; i <= 8; ++i) {
                if (f.read_file("o" + std::to_string(i) + ".txt") == "v1\n") {
                    ++stranded;
                }
            }
            INFO("stranded commands: " << stranded);
            REQUIRE(stranded > 0);

            THEN("the next build runs every command the abort skipped")
            {
                f.remove_file("gate");
                auto again = f.build();
                INFO("stdout: " << again.stdout_output);
                INFO("stderr: " << again.stderr_output);
                REQUIRE(again.success());
                for (auto i = 1; i <= 8; ++i) {
                    INFO("output " << i);
                    REQUIRE(f.read_file("o" + std::to_string(i) + ".txt") == "v2\n");
                }
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

SCENARIO("A build that cannot save its record does not report success", "[e2e][incremental]")
{
    GIVEN("an in-tree project whose record path is blocked by a directory")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        // Renaming onto a directory fails for root too, so this needs no permission bits and
        // no skip guard, unlike the scenarios that revoke write access to a directory.
        f.mkdir(".pup/index");

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the commands ran, and the build fails rather than reporting work it did not record")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(f.read_file("out.txt") == "ORIGINAL\n");
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("build record") != std::string::npos);
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

namespace {

auto index_bytes(E2EFixture const& f) -> std::vector<std::byte>
{
    auto in = std::ifstream { f.workdir() / ".pup" / "index", std::ios::binary };
    auto chars = std::vector<char> { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    auto bytes = std::vector<std::byte> (chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

auto write_index_bytes(E2EFixture const& f, std::vector<std::byte> const& bytes) -> void
{
    auto out = std::ofstream { f.workdir() / ".pup" / "index", std::ios::binary | std::ios::trunc };
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Restamp the build record's format version and re-sign it, so it reads as one an older putup
/// wrote. Only the version differs: the file table is the part every version since the floor
/// shares, and it is all the recovery read looks at.
auto stamp_index_version(E2EFixture const& f, std::uint32_t version) -> void
{
    auto bytes = index_bytes(f);
    REQUIRE(bytes.size() > sizeof(pup::index::RawHeader) + sizeof(pup::index::RawFooter));

    auto* hdr = reinterpret_cast<pup::index::RawHeader*>(bytes.data());
    hdr->version = version;

    auto const content_size = bytes.size() - sizeof(pup::index::RawFooter);
    auto const checksum = pup::sha256(std::span<std::byte const> { bytes.data(), content_size });
    std::memcpy(bytes.data() + content_size, checksum.data(), checksum.size());

    write_index_bytes(f, bytes);
}

auto truncate_index(E2EFixture const& f, std::uintmax_t size) -> void
{
    std::filesystem::resize_file(f.workdir() / ".pup" / "index", size);
}

/// Damage the record the way only its own header can say: a section declared past the end of the
/// file, re-signed so the checksum still passes and the layout check is what refuses it.
auto damage_index_layout(E2EFixture const& f) -> void
{
    auto bytes = index_bytes(f);
    REQUIRE(bytes.size() > sizeof(pup::index::RawHeader) + sizeof(pup::index::RawFooter));

    auto* hdr = reinterpret_cast<pup::index::RawHeader*>(bytes.data());
    hdr->file_offset = static_cast<std::uint32_t>(bytes.size());

    auto const content_size = bytes.size() - sizeof(pup::index::RawFooter);
    auto const checksum = pup::sha256(std::span<std::byte const> { bytes.data(), content_size });
    std::memcpy(bytes.data() + content_size, checksum.data(), checksum.size());

    write_index_bytes(f, bytes);
}

/// Everything a record says about *what* the build is, with the two fields that say *when* it
/// happened left out: the header's save_time_ns and each file's mtime_ns are wall-clock
/// observations, so two builds minutes apart differ there by design. Section bounds come from the
/// header rather than hard-coded offsets, so a header that grows a field does not silently turn
/// this into a comparison of the wrong bytes; the footer is excluded because it is the checksum
/// over the timestamp we just masked.
auto index_shape(std::vector<std::byte> const& bytes) -> std::vector<std::byte>
{
    REQUIRE(bytes.size() > sizeof(pup::index::RawHeader) + sizeof(pup::index::RawFooter));

    auto hdr = pup::index::RawHeader {};
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));
    hdr.save_time_ns = 0;

    auto shape = std::vector<std::byte> {};
    auto append = [&](void const* src, std::size_t n) {
        auto const* p = static_cast<std::byte const*>(src);
        shape.insert(shape.end(), p, p + n);
    };
    append(&hdr, sizeof(hdr));

    for (auto i = std::uint32_t { 0 }; i < hdr.file_count; ++i) {
        auto entry = pup::index::RawFileEntry {};
        std::memcpy(&entry, bytes.data() + hdr.file_offset + i * sizeof(entry), sizeof(entry));
        entry.mtime_ns = 0;
        append(&entry, sizeof(entry));
    }

    append(bytes.data() + hdr.command_offset, std::size_t { hdr.command_count } * sizeof(pup::index::RawCommandEntry));
    append(bytes.data() + hdr.edge_offset, std::size_t { hdr.edge_count } * sizeof(pup::index::RawEdge));
    append(bytes.data() + hdr.operand_table_offset, std::size_t { hdr.command_count } * sizeof(std::uint32_t));
    append(bytes.data() + hdr.operand_data_offset, hdr.string_offset - hdr.operand_data_offset);
    append(bytes.data() + hdr.string_offset, hdr.string_table_size);

    return shape;
}

} // namespace

// Determinism of construction, not of reload: two builds that never saw each other's work must
// record the same project. Answering this by reading the code is what #298's consult had to do.
SCENARIO("Two builds of one tree record the same thing", "[e2e][index][determinism]")
{
    GIVEN("the same project built twice from scratch, in trees that share nothing")
    {
        auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

        auto first = E2EFixture { "implicit_deps" };
        REQUIRE(first.init().success());
        REQUIRE(first.build().success());

        auto second = E2EFixture { "implicit_deps" };
        REQUIRE(second.init().success());
        REQUIRE(second.build().success());

        WHEN("the two records are compared")
        {
            auto const a = index_bytes(first);
            auto const b = index_bytes(second);

            THEN("they agree on every entry, edge, operand and string")
            {
                REQUIRE(index_shape(a) == index_shape(b));
            }

            // A record whose sections are identical but whose lengths are not would mean the
            // masking above is hiding a difference rather than excluding a timestamp; a shape
            // that covers only the header would mean it is comparing almost nothing.
            THEN("they are the same size, and the comparison covers the record")
            {
                REQUIRE(a.size() == b.size());
                REQUIRE(index_shape(a).size() > a.size() / 2);
            }
        }

        // The weaker property the incremental suite leans on, asserted here because it costs one
        // build: a run that does nothing must not rewrite what the record says either.
        WHEN("one of them is rebuilt with nothing to do")
        {
            auto const before = index_bytes(first);
            REQUIRE(first.build().is_noop());
            auto const after = index_bytes(first);

            THEN("the record it rewrites says the same thing")
            {
                REQUIRE(index_shape(before) == index_shape(after));
            }
        }
    }
}

SCENARIO("An index an older putup wrote does not turn an in-tree build's own outputs into source collisions", "[e2e][shadow][incremental]")
{
    GIVEN("an in-tree project built once, so its output is on disk beside the sources")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        REQUIRE(f.exists("out.txt"));

        WHEN("the record carries the previous format version and the project is rebuilt")
        {
            stamp_index_version(f, pup::index::INDEX_VERSION - 1);
            auto again = f.build();

            THEN("the build succeeds: a record too old to trust still says which files it made")
            {
                INFO("stdout: " << again.stdout_output);
                INFO("stderr: " << again.stderr_output);
                REQUIRE(again.success());
                REQUIRE(f.read_file("out.txt") == "ORIGINAL\n");
            }
        }
    }
}

SCENARIO("Cleaning removes the outputs a record too old to trust still names", "[e2e][clean][shadow]")
{
    GIVEN("an in-tree project built once against a record from an older putup")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        stamp_index_version(f, pup::index::INDEX_VERSION - 1);

        WHEN("the project is cleaned")
        {
            auto cleaned = f.clean();

            THEN("the generated file is removed and the source is not")
            {
                INFO("stdout: " << cleaned.stdout_output);
                INFO("stderr: " << cleaned.stderr_output);
                REQUIRE_FALSE(f.exists("out.txt"));
                REQUIRE(f.exists("src.txt"));
            }
        }
    }
}

SCENARIO("Cleaning a record it cannot read fails instead of reporting nothing to clean", "[e2e][clean]")
{
    GIVEN("an in-tree project built once")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        REQUIRE(f.exists("out.txt"));

        WHEN("the record is stamped below the readable window and the project is cleaned")
        {
            stamp_index_version(f, pup::index::INDEX_LAYOUT_FLOOR - 1);
            auto cleaned = f.clean();

            THEN("it fails and names the record, rather than reporting an empty clean")
            {
                INFO("stdout: " << cleaned.stdout_output);
                INFO("stderr: " << cleaned.stderr_output);
                REQUIRE_FALSE(cleaned.success());
                REQUIRE(cleaned.stderr_output.find("cannot be read") != std::string::npos);
                REQUIRE(f.exists("out.txt"));
            }
        }
    }
}

SCENARIO("Distcleaning a record it cannot read does not report a complete reset", "[e2e][clean]")
{
    GIVEN("an in-tree project built once")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        REQUIRE(f.exists("out.txt"));

        WHEN("the record is stamped below the readable window and the project is reset")
        {
            stamp_index_version(f, pup::index::INDEX_LAYOUT_FLOOR - 1);
            auto reset = f.distclean();

            THEN("it fails and does not claim the reset it left half done")
            {
                INFO("stdout: " << reset.stdout_output);
                INFO("stderr: " << reset.stderr_output);
                REQUIRE_FALSE(reset.success());
                REQUIRE(reset.stdout_output.find("Project reset complete") == std::string::npos);
                REQUIRE(f.exists("out.txt"));
            }
        }
    }
}

SCENARIO("Distcleaning keeps a record it cannot read and resets the rest", "[e2e][clean]")
{
    GIVEN("an in-tree project built once")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());

        WHEN("the record is stamped below the readable window and the project is reset")
        {
            stamp_index_version(f, pup::index::INDEX_LAYOUT_FLOOR - 1);
            auto reset = f.distclean();

            THEN("the record survives, the rest of the reset happens, and the way out is named")
            {
                INFO("stdout: " << reset.stdout_output);
                INFO("stderr: " << reset.stderr_output);
                REQUIRE_FALSE(reset.success());
                REQUIRE(f.exists(".pup/index"));
                REQUIRE_FALSE(f.exists("tup.config"));
                auto combined = reset.stdout_output + reset.stderr_output;
                REQUIRE(combined.find("rm -rf") != std::string::npos);
            }
        }
    }
}

SCENARIO("A record distclean kept can still name the files it owns", "[e2e][clean]")
{
    GIVEN("a project whose reset was refused because its record could not be read")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        stamp_index_version(f, pup::index::INDEX_LAYOUT_FLOOR - 1);
        REQUIRE_FALSE(f.distclean().success());
        REQUIRE(f.exists(".pup/index"));
        REQUIRE(f.exists("out.txt"));

        WHEN("a putup whose window covers that record cleans the project")
        {
            // The same bytes, a reader that accepts them: what keeping the record buys.
            stamp_index_version(f, pup::index::INDEX_VERSION);
            auto cleaned = f.clean();

            THEN("the file the refused reset could not name is removed after all")
            {
                INFO("stdout: " << cleaned.stdout_output);
                INFO("stderr: " << cleaned.stderr_output);
                REQUIRE(cleaned.success());
                REQUIRE_FALSE(f.exists("out.txt"));
                REQUIRE(f.exists("src.txt"));
            }
        }
    }
}

SCENARIO("A distclean dry run predicts the record it would keep", "[e2e][clean][dry-run]")
{
    GIVEN("an in-tree project built once, with a record below the readable window")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        stamp_index_version(f, pup::index::INDEX_LAYOUT_FLOOR - 1);

        WHEN("the reset is dry-run")
        {
            auto reset = f.distclean({ "-n" });

            THEN("it does not offer to remove the record it would keep")
            {
                INFO("stdout: " << reset.stdout_output);
                INFO("stderr: " << reset.stderr_output);
                REQUIRE_FALSE(reset.success());
                REQUIRE(reset.stdout_output.find("Would remove: ") != std::string::npos);
                REQUIRE(reset.stdout_output.find(".pup\n") == std::string::npos);
                REQUIRE(f.exists(".pup/index"));
            }
        }
    }
}

SCENARIO("A damaged record says so instead of rebuilding in silence", "[e2e][incremental]")
{
    GIVEN("an in-tree project built once, whose record declares a section past the end of the file")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        damage_index_layout(f);
        // Without this the shadow guard speaks first: an output on disk with no readable record
        // is the #291 refusal, which would pass this scenario for the wrong reason.
        f.remove_file("out.txt");

        WHEN("the next build loads that record")
        {
            auto const result = f.build();

            THEN("it names the damage rather than reading as a first build")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("is damaged") != std::string::npos);
                REQUIRE(result.success());
                REQUIRE(f.exists("out.txt"));
            }
        }
    }
}

SCENARIO("A record too short to hold a header says so instead of rebuilding in silence", "[e2e][incremental]")
{
    GIVEN("an in-tree project built once, whose record is then cut below header and footer")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        // Below sizeof(RawHeader) + sizeof(RawFooter): above it the declared-layout row answers
        // first and this would pin the wrong rejection.
        truncate_index(f, 40);
        // Without this the shadow guard speaks first, as in the layout scenario above.
        f.remove_file("out.txt");

        WHEN("the next build loads that record")
        {
            auto const result = f.build();

            THEN("it names the damage rather than reading as a first build")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("is damaged") != std::string::npos);
                REQUIRE(result.success());
                REQUIRE(f.exists("out.txt"));
            }
        }
    }
}

SCENARIO("A record that cannot be opened is announced without being called damage", "[e2e][incremental]")
{
    GIVEN("an in-tree project built once, with a directory standing where its record belongs")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());

        auto const index_path = f.workdir() / ".pup" / "index";
        std::filesystem::remove(index_path);
        std::filesystem::create_directory(index_path);
        f.remove_file("out.txt");

        WHEN("the next build tries to load it")
        {
            auto const result = f.build();

            THEN("it says the record could not be read, and does not claim the record is damaged")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("could not be read") != std::string::npos);
                REQUIRE(result.stderr_output.find("damaged") == std::string::npos);
            }
        }
    }
}

SCENARIO("A rule writing above the build root fails the build instead of overwriting the file there", "[e2e][build][hierarchy]")
{
    GIVEN("an out-of-tree project whose rule writes a path that climbs out of the build directory")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", "");
        f.write_file("src.txt", "GENERATED\n");
        f.write_file("victim.txt", "A COMMITTED SOURCE\n");
        REQUIRE(f.pup({ "configure", "-B", "bld" }).success());
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> ../victim.txt\n");

        WHEN("the project is built")
        {
            auto const result = f.build({ "-B", "bld" });

            THEN("the build fails and the file that was already there is untouched")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("hierarchy") != std::string::npos);
                REQUIRE(f.read_file("victim.txt") == "A COMMITTED SOURCE\n");
            }
        }
    }
}

SCENARIO("A parent reference inside the tree is recorded canonically", "[e2e][build][hierarchy]")
{
    GIVEN("an in-tree project whose subdirectory rule writes into its parent")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", "");
        f.mkdir("sub");
        f.write_file("sub/Tupfile", ": src.txt |> cp %f %o |> ../gen.txt\n");
        f.write_file("sub/src.txt", "SEED\n");

        WHEN("the project is built")
        {
            auto const result = f.build();

            THEN("it builds, and the record names the output by where it landed")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("gen.txt"));

                auto const bytes = index_bytes(f);
                auto const* hdr = reinterpret_cast<pup::index::RawHeader const*>(bytes.data());
                auto name_at = [&bytes, hdr](std::uint32_t offset) {
                    auto const at = std::size_t { hdr->string_offset } + offset;
                    auto length = std::uint16_t {};
                    std::memcpy(&length, bytes.data() + at, sizeof(length));
                    return std::string { reinterpret_cast<char const*>(bytes.data()) + at + sizeof(length), length };
                };

                auto generated = std::vector<std::string> {};
                auto parent_refs = 0;
                for (auto i = std::uint32_t { 0 }; i < hdr->file_count; ++i) {
                    auto entry = pup::index::RawFileEntry {};
                    std::memcpy(&entry, bytes.data() + hdr->file_offset + i * sizeof(entry), sizeof(entry));
                    auto const name = name_at(entry.name_offset);
                    if (name == "..") {
                        ++parent_refs;
                    }
                    if (entry.type == static_cast<std::uint8_t>(pup::NodeType::Generated)) {
                        generated.push_back(name);
                    }
                }

                REQUIRE(parent_refs == 0);
                REQUIRE(generated == std::vector<std::string> { "gen.txt" });
            }
        }
    }
}

SCENARIO("An index from an unsupported version rebuilds without calling it damage", "[e2e][incremental]")
{
    GIVEN("an in-tree project built once, with a record a newer putup wrote")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());
        stamp_index_version(f, pup::index::INDEX_VERSION + 1);
        f.remove_file("out.txt");

        WHEN("the next build loads that record")
        {
            auto const result = f.build();

            THEN("it stays quiet: a version outside the window is expected, not damage")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("is damaged") == std::string::npos);
                REQUIRE(result.success());
                REQUIRE(f.exists("out.txt"));
            }
        }
    }
}

SCENARIO("Distcleaning keeps a record whose files it could not remove", "[e2e][clean]")
{
    GIVEN("an in-tree project whose generated file sits in a directory that cannot be written")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> echo x > %o |> out/gen.o\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("out/gen.o"));

        auto const dir = f.workdir() / "out";
        auto const writable = std::filesystem::status(dir).permissions();
        std::filesystem::permissions(
            dir,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace
        );
        auto probe_ec = std::error_code {};
        std::filesystem::create_directory(dir / "probe", probe_ec);
        if (!probe_ec) {
            std::filesystem::remove(dir / "probe", probe_ec);
            std::filesystem::permissions(dir, writable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke write permission (running as root?): scenario not exercised");
            return;
        }

        WHEN("the project is reset")
        {
            auto result = f.distclean({ "-v" });
            std::filesystem::permissions(dir, writable, std::filesystem::perm_options::replace);

            THEN("the record survives, so the file it owns stays attributable")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                // The removal failure names the file on its own, so only the keep message
                // witnesses that the record was kept because of it.
                REQUIRE(combined.find("Keeping the build record") != std::string::npos);
                REQUIRE(combined.find("rm -rf") != std::string::npos);
                REQUIRE(combined.find("Project reset complete") == std::string::npos);
                REQUIRE(f.exists("out/gen.o"));
                REQUIRE(f.exists(".pup/index"));
                REQUIRE_FALSE(f.exists("tup.config"));
                REQUIRE_FALSE(result.success());
            }
        }
    }
}

SCENARIO("Distcleaning out of tree keeps a record whose files it could not remove", "[e2e][clean][variant]")
{
    GIVEN("an out-of-tree project whose generated file sits in a directory that cannot be written")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": |> echo x > %o |> out/gen.o\n");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        REQUIRE(f.exists("build/out/gen.o"));

        auto const dir = f.workdir() / "build" / "out";
        auto const writable = std::filesystem::status(dir).permissions();
        std::filesystem::permissions(
            dir,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace
        );
        auto probe_ec = std::error_code {};
        std::filesystem::create_directory(dir / "probe", probe_ec);
        if (!probe_ec) {
            std::filesystem::remove(dir / "probe", probe_ec);
            std::filesystem::permissions(dir, writable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke write permission (running as root?): scenario not exercised");
            return;
        }

        WHEN("the project is reset")
        {
            auto result = f.distclean({ "-B", "build", "-v" });
            std::filesystem::permissions(dir, writable, std::filesystem::perm_options::replace);

            THEN("the record survives there too: the rule is about the leftover, not about being in tree")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("Keeping the build record") != std::string::npos);
                REQUIRE(combined.find("rm -rf") != std::string::npos);
                REQUIRE(combined.find("Project reset complete") == std::string::npos);
                REQUIRE(f.exists("build/out/gen.o"));
                REQUIRE(f.exists("build/.pup/index"));
                REQUIRE_FALSE(f.exists("build/tup.config"));
                REQUIRE_FALSE(result.success());
            }
        }
    }
}

SCENARIO("A record older than the readable window is refused, not misread", "[e2e][shadow][incremental]")
{
    GIVEN("an in-tree project built once")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": src.txt |> cp %f %o |> out.txt\n");
        f.write_file("src.txt", "ORIGINAL\n");
        REQUIRE(f.build().success());

        WHEN("the record carries a version whose layout putup no longer knows")
        {
            stamp_index_version(f, pup::index::INDEX_LAYOUT_FLOOR - 1);
            auto again = f.build();

            THEN("the build stops and says the record is unreadable rather than blaming the Tupfile")
            {
                INFO("stdout: " << again.stdout_output);
                INFO("stderr: " << again.stderr_output);
                REQUIRE_FALSE(again.success());
                REQUIRE(again.stderr_output.find("out.txt") != std::string::npos);
                REQUIRE(again.stderr_output.find("cannot be read") != std::string::npos);
                REQUIRE(f.exists("out.txt"));
            }
        }
    }
}

SCENARIO("A damaged build record does not silently disarm the source-file guard", "[e2e][shadow]")
{
    GIVEN("a built in-tree project whose Tupfile then starts generating a file it used to read")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": keep.txt |> cp %f %o |> out.txt\n");
        f.write_file("keep.txt", "PRECIOUS\n");
        REQUIRE(f.build().success());
        f.write_file("Tupfile",
            ": keep.txt |> cp %f %o |> out.txt\n"
            ": src.txt |> cp %f %o |> keep.txt\n");
        f.write_file("src.txt", "CLOBBERED\n");

        WHEN("the record is truncated, so it no longer describes the tree it came from")
        {
            truncate_index(f, 100);
            auto result = f.build();

            THEN("the build is rejected and the checked-in file survives")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(f.read_file("keep.txt") == "PRECIOUS\n");
            }
        }
    }
}

SCENARIO("Every shadowed source is named, not just the first", "[e2e][shadow]")
{
    GIVEN("two rules whose outputs are both checked-in files")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": gen.src |> cp %f %o |> a.dat\n"
            ": gen.src |> cp %f %o |> b.dat\n");
        f.write_file("gen.src", "FROMRULE\n");
        f.write_file("a.dat", "FROMSRC\n");
        f.write_file("b.dat", "FROMSRC\n");

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("one build names both, so the remedy does not have to be repeated per file")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("a.dat") != std::string::npos);
                REQUIRE(result.stderr_output.find("b.dat") != std::string::npos);
                REQUIRE(f.read_file("a.dat") == "FROMSRC\n");
                REQUIRE(f.read_file("b.dat") == "FROMSRC\n");
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

SCENARIO("An exclusion that is itself a glob applies to generated matches out-of-tree", "[e2e][glob][pathspace][exclusion]")
{
    GIVEN("a glob over generated files excluded by a pattern rather than a literal")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": keep.in |> cp %f %o |> keep.gen\n"
            ": skip.in |> cp %f %o |> skip.gen\n"
            ": *.gen !s*.gen |> echo %f > %o |> out.txt\n");
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

SCENARIO("A glob exclusion's match set does not depend on what exists on disk", "[e2e][glob][pathspace][exclusion]")
{
    GIVEN("a pattern exclusion over files the build itself generates")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": keep.in |> cp %f %o |> keep.gen\n"
            ": skip.in |> cp %f %o |> skip.gen\n"
            ": *.gen !s*.gen |> echo %f > %o |> out.txt\n");
        f.write_file("keep.in", "k\n");
        f.write_file("skip.in", "s\n");
        REQUIRE(f.init().success());

        WHEN("built twice in-tree, so the second parse sees files the first did not")
        {
            REQUIRE(f.build().success());
            auto first = f.read_file("out.txt");

            REQUIRE(f.build().success());
            auto second = f.read_file("out.txt");

            THEN("both builds exclude the pattern's matches and agree")
            {
                INFO("build 1: " << first);
                INFO("build 2: " << second);
                REQUIRE(first.find("skip.gen") == std::string::npos);
                REQUIRE(first == second);
            }
        }
    }
}

SCENARIO("An exclusion applies to bin members out-of-tree", "[e2e][glob][pathspace][exclusion][bin]")
{
    GIVEN("a rule consuming a bin with one member excluded")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": x.in |> cp %f %o |> x.dat {bin}\n"
            ": y.in |> cp %f %o |> y.dat {bin}\n"
            ": {bin} !x.dat |> cat %f > %o |> out.txt\n");
        f.write_file("x.in", "x\n");
        f.write_file("y.in", "y\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built out-of-tree")
        {
            REQUIRE(f.build({ "-B", "build" }).success());

            THEN("the excluded member is absent, and the surviving one is readable")
            {
                auto content = f.read_file("build/out.txt");
                INFO("out.txt: " << content);
                REQUIRE(content == "y\n");
            }
        }
    }
}

SCENARIO("An exclusion does not erase a group reference or the pattern it filters", "[e2e][glob][pathspace][exclusion]")
{
    GIVEN("a rule whose inputs are a group reference and a glob, with a matching exclusion")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("sub");
        f.write_file("sub/Tupfile", ": |> echo g > %o |> gen.h | <hdrs>\n");
        f.write_file("Tupfile", ": sub/<hdrs> sub/*.c !sub/* |> echo F=[%f] > %o |> out.txt\n");
        REQUIRE(f.init().success());

        WHEN("built with an exclusion that also matches those non-file entries")
        {
            REQUIRE(f.build().success());

            THEN("the rule still runs")
            {
                REQUIRE(f.exists("out.txt"));
            }
        }
    }
}

SCENARIO("A glob exclusion matches a hidden file the same as any other", "[e2e][glob][pathspace][exclusion]")
{
    GIVEN("an explicitly named hidden input alongside ordinary ones")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": .foo.c bar.c other.txt !*.c |> echo F=[%f] > %o |> out.txt\n");
        f.write_file(".foo.c", "h\n");
        f.write_file("bar.c", "b\n");
        f.write_file("other.txt", "o\n");
        REQUIRE(f.init().success());

        WHEN("built with an exclusion matching the .c extension")
        {
            REQUIRE(f.build().success());

            THEN("the hidden file is excluded like the rest")
            {
                auto content = f.read_file("out.txt");
                INFO("out.txt: " << content);
                REQUIRE(content == "F=[other.txt]\n");
            }
        }
    }
}

SCENARIO("An exclusion matches an input spelled with the build-directory prefix", "[e2e][glob][pathspace][exclusion]")
{
    GIVEN("a generated input named build-prefixed alongside an exclusion that covers it")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile",
            ": foo.in |> cp %f %o |> foo.gen\n"
            ": keep.txt build/foo.gen !*.gen |> echo F=[%f] > %o |> out.txt\n");
        f.write_file("foo.in", "f\n");
        f.write_file("keep.txt", "k\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built out-of-tree")
        {
            REQUIRE(f.build({ "-B", "build" }).success());

            THEN("the exclusion removes it, as it does for the unprefixed spelling")
            {
                auto content = f.read_file("build/out.txt");
                INFO("out.txt: " << content);
                REQUIRE(content == "F=[keep.txt]\n");
            }
        }
    }
}

SCENARIO("A foreach rule substitutes %g in its command, not only in its output name", "[e2e][glob][foreach]")
{
    GIVEN("a foreach rule using %g on both sides")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("Tupfile", ": foreach *_test.c |> echo MATCH-%g-END > %o |> %g_result.txt\n");
        f.write_file("foo_test.c", "a\n");
        f.write_file("bar_test.c", "b\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built")
        {
            REQUIRE(f.build({ "-B", "build" }).success());

            THEN("the command sees the same match the output name did")
            {
                REQUIRE(f.exists("build/foo_result.txt"));
                REQUIRE(f.exists("build/bar_result.txt"));
                INFO("foo_result.txt: " << f.read_file("build/foo_result.txt"));
                REQUIRE(f.read_file("build/foo_result.txt") == "MATCH-foo-END\n");
                REQUIRE(f.read_file("build/bar_result.txt") == "MATCH-bar-END\n");
            }
        }
    }
}

SCENARIO("A glob sees files generated by a directory parsed after it", "[e2e][glob][traversal]")
{
    GIVEN("a rule globbing files that a later-parsed sibling generates")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("b");
        f.mkdir("m");
        f.mkdir("n");
        f.write_file("m/Tupfile", ": ../b/*.q |> cat %f > %o |> out.txt\n");
        f.write_file("n/Tupfile", ": |> echo x > %o |> ../b/gen.q\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built")
        {
            auto result = f.build({ "-B", "build" });
            REQUIRE(result.success());

            THEN("the generated file is an input, and the build settles")
            {
                INFO("out.txt: " << f.read_file("build/m/out.txt"));
                REQUIRE(f.read_file("build/m/out.txt") == "x\n");
                REQUIRE(f.build({ "-B", "build" }).is_noop());
            }
        }
    }
}

SCENARIO("A foreach glob instantiates a command for a later-generated file", "[e2e][glob][traversal][foreach]")
{
    GIVEN("a foreach rule over files a later-parsed sibling generates")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("b");
        f.mkdir("m");
        f.mkdir("n");
        f.write_file("m/Tupfile", ": foreach ../b/*.q |> cat %f > %o |> %B.txt\n");
        f.write_file("n/Tupfile", ": |> echo x > %o |> ../b/gen.q\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built")
        {
            auto result = f.build({ "-B", "build" });
            REQUIRE(result.success());

            THEN("the per-file command exists and ran")
            {
                REQUIRE(f.exists("build/m/gen.txt"));
                REQUIRE(f.read_file("build/m/gen.txt") == "x\n");
            }
        }
    }
}

SCENARIO("A glob's match set does not depend on the producing directory's name", "[e2e][glob][traversal]")
{
    GIVEN("one project built twice, the producer named to sort before then after the consumer")
    {
        auto build_with = [](std::string_view producer) {
            auto f = E2EFixture { "glob_mixed_space" };
            f.mkdir("b");
            f.mkdir("m");
            f.mkdir(std::string { producer });
            f.write_file("m/Tupfile", ": ../b/*.q |> cat %f > %o |> out.txt\n");
            f.write_file(std::string { producer } + "/Tupfile", ": |> echo x > %o |> ../b/gen.q\n");
            f.mkdir("build");
            REQUIRE(f.pup({ "configure", "-B", "build" }).success());
            REQUIRE(f.build({ "-B", "build" }).success());
            return f.read_file("build/m/out.txt");
        };

        WHEN("the producer sorts before and after the consumer")
        {
            auto before = build_with("a");
            auto after = build_with("n");

            THEN("both produce the same artifact")
            {
                INFO("producer a/: " << before);
                INFO("producer n/: " << after);
                REQUIRE(before == "x\n");
                REQUIRE(before == after);
            }
        }
    }
}

SCENARIO("A glob whose producer is parsed first is unaffected", "[e2e][glob][traversal]")
{
    GIVEN("the same project with the producing directory sorting before the consumer")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("b");
        f.mkdir("m");
        f.mkdir("a");
        f.write_file("m/Tupfile", ": ../b/*.q |> cat %f > %o |> out.txt\n");
        f.write_file("a/Tupfile", ": |> echo x > %o |> ../b/gen.q\n");
        f.mkdir("build");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());

        WHEN("built")
        {
            auto result = f.build({ "-B", "build" });

            THEN("the stability check does not fire and the artifact is correct")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("build/m/out.txt") == "x\n");
            }
        }
    }
}

SCENARIO("Deleting a stale output re-runs its order-only consumer in the same build", "[e2e][stale][order-only]")
{
    GIVEN("a consumer that depends on a generated file order-only")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("a/Tupfile", ": |> echo hi > %o |> gen.txt\n");
        f.write_file("b/Tupfile", ": | ../a/gen.txt |> ls ../a > %o |> listing.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("b/listing.txt").find("gen.txt") != std::string::npos);

        WHEN("the producing rule is dropped, so the file becomes stale and is deleted")
        {
            f.write_file("a/Tupfile", "");
            auto result = f.build();
            REQUIRE(result.success());

            THEN("the consumer re-runs in that build rather than the next one")
            {
                REQUIRE_FALSE(f.exists("a/gen.txt"));
                INFO("listing.txt: " << f.read_file("b/listing.txt"));
                REQUIRE(f.read_file("b/listing.txt").find("gen.txt") == std::string::npos);
            }
        }
    }
}

SCENARIO("A glob consumer of a deleted stale output settles after the healing build", "[e2e][stale][order-only]")
{
    // The heal in build 2 is #212 and is deliberately better than tup, which runs the consumer
    // zero times; only the third build is the defect (#213).
    GIVEN("a consumer that reaches a generated file through an order-only glob")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("a/Tupfile", ": |> echo hi > %o |> gen.txt\n");
        f.write_file("b/Tupfile", ": | ../a/*.txt |> ls ../a > %o |> listing.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("b/listing.txt").find("gen.txt") != std::string::npos);

        WHEN("the producing rule is dropped and the healing build has run")
        {
            f.write_file("a/Tupfile", "");
            auto heal = f.build();
            REQUIRE(heal.success());
            REQUIRE_FALSE(heal.is_noop());
            REQUIRE_FALSE(f.exists("a/gen.txt"));

            THEN("the build after it has nothing left to do")
            {
                auto settled = f.build();
                INFO("stdout: " << settled.stdout_output);
                REQUIRE(settled.success());
                REQUIRE(settled.is_noop());
            }

            AND_WHEN("the deleted file is put back by hand")
            {
                f.write_file("a/gen.txt", "recreated\n");
                auto again = f.build();

                THEN("the consumer runs again: an absence already routed is not a licence to stop looking")
                {
                    INFO("stdout: " << again.stdout_output);
                    REQUIRE(again.success());
                    REQUIRE_FALSE(again.is_noop());
                    REQUIRE(f.read_file("b/listing.txt").find("gen.txt") != std::string::npos);
                }
            }
        }
    }
}

SCENARIO("A rule still naming a deleted stale output is rejected rather than re-run", "[e2e][stale][order-only]")
{
    // The first rebuild is #212's routed heal; the defect is the build after it, which re-detects the file putup itself deleted (#213).
    GIVEN("a consumer whose order-only input was generated by a rule that has been dropped")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("a/Tupfile", ": |> echo hi > %o |> gen.txt\n");
        f.write_file("b/Tupfile", ": | ../a/gen.txt |> ls ../a > %o |> listing.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        f.write_file("a/Tupfile", "");
        REQUIRE(f.build().success());
        REQUIRE_FALSE(f.exists("a/gen.txt"));

        WHEN("the project is built again, with the input still named and nothing producing it")
        {
            auto result = f.build();

            THEN("the build fails rather than running the consumer a second time")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("unresolved ghost") != std::string::npos);
                REQUIRE(combined.find("a/gen.txt") != std::string::npos);
            }
        }
    }
}

SCENARIO("Removing a group member re-runs the commands that consume the group", "[e2e][stale][order-only][group]")
{
    // The rule vanishes with its glob match, never by a Tupfile edit: an edit rebuilds a surviving member, and a rebuilt member reaches the consumer over the live graph, masking the removal (#169).
    GIVEN("a consumer that depends on a group order-only and reads the directory for members")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("a/Tupfile", ": foreach *.txt |> cp %f %o |> %B.o <objs>\n");
        f.write_file("a/one.txt", "x\n");
        f.write_file("a/two.txt", "x\n");
        f.write_file("b/Tupfile", ": | ../a/<objs> |> ls ../a > %o |> listing.txt\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("b/listing.txt").find("two.o") != std::string::npos);

        WHEN("one member's rule vanishes and no surviving member is rebuilt")
        {
            f.remove_file("a/two.txt");
            auto result = f.build({ "-v" });
            REQUIRE(result.success());

            THEN("the consumer re-runs, routed by the removed member itself")
            {
                REQUIRE_FALSE(f.exists("a/two.o"));
                INFO("listing.txt: " << f.read_file("b/listing.txt"));
                REQUIRE(f.read_file("b/listing.txt").find("two.o") == std::string::npos);
                REQUIRE(f.read_file("b/listing.txt").find("one.o") != std::string::npos);
                INFO("stdout: " << result.stdout_output);
                // Names the consuming command too, which an unannotated rule did not (#229).
                REQUIRE(result.stdout_output.find("Removed input: a/two.o (ls ../a") != std::string::npos);
                // The removed foreach instance, not the pattern its sibling also matches.
                REQUIRE(result.stdout_output.find("Removed command: cp two.txt two.o (in a)") != std::string::npos);
            }
        }
    }
}

SCENARIO("Removing a group member schedules nothing when the group's only consumer is guarded off", "[e2e][stale][order-only][group]")
{
    // Edges into a guard-unsatisfied command are dropped when the index is written, so the walk finding nothing here is the correct answer and not a missed route.
    GIVEN("a group whose only consumer sits in an inactive conditional")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("a");
        f.mkdir("b");
        f.write_file("a/Tupfile", ": foreach *.txt |> cp %f %o |> %B.o <objs>\n");
        f.write_file("a/one.txt", "x\n");
        f.write_file("a/two.txt", "x\n");
        f.write_file("b/Tupfile", "ifdef WANT_LISTING\n: | ../a/<objs> |> ls ../a > %o |> listing.txt\nendif\n");
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE_FALSE(f.exists("b/listing.txt"));

        WHEN("a member's rule vanishes with its glob match")
        {
            f.remove_file("a/two.txt");
            auto result = f.build({ "-v" });

            THEN("the build succeeds and routes the removal to no command")
            {
                REQUIRE(result.success());
                REQUIRE_FALSE(f.exists("a/two.o"));
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("Removed input:") == std::string::npos);
                REQUIRE_FALSE(f.exists("b/listing.txt"));
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
                // main.c lives in source_dir, not source_dir/tupfiles, so a rule for its object exists only if the glob matched it there.
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.stdout_output.find("main.o") != std::string::npos);
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

SCENARIO("The object of a compile that runs elsewhere is reported instead of scanned wrongly", "[e2e][strict][depscan]")
{
    // The scan runs from the Tupfile's directory, so a source word taken from an invocation that
    // ran in sub/ resolves against a same-named file here -- deps recorded for a file the rule
    // never compiled (#356).
    GIVEN("a rule that compiles here and then again after a cd, with a same-named source in both")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.mkdir("sub");
        f.write_file("a.c", "#include \"root.h\"\nint a(void){return 0;}\n");
        f.write_file("b.c", "#include \"decoy.h\"\nint decoy(void){return 0;}\n");
        f.write_file("sub/b.c", "#include \"subonly.h\"\nint b(void){return 1;}\n");
        f.write_file("root.h", "#define R 1\n");
        f.write_file("decoy.h", "#define D 1\n");
        f.write_file("sub/subonly.h", "#define S 1\n");
        f.write_file("Tupfile", ": a.c |> gcc -c a.c -o a.o && cd sub && gcc -c b.c -o b.o |> a.o sub/b.o\n");
        REQUIRE(f.build().success());

        WHEN("parse reports on the rule")
        {
            auto result = f.pup({ "parse" });

            THEN("it names the unreachable object and leaves the reproducible one scanned")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("no dependency scan") != std::string::npos);
                REQUIRE(result.stderr_output.find("sub/b.o") != std::string::npos);
                REQUIRE(result.stderr_output.find("'a.o'") == std::string::npos);
            }

            THEN("it does not claim the command contains no reproducible compile")
            {
                // One is standing next to it: a.o's compile is the covered prefix. The report's
                // unit moved to the object, so its sentence has to speak about the object.
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("in this command") == std::string::npos);
                REQUIRE(
                    result.stderr_output.find("no compile putup can reproduce writes")
                    != std::string::npos
                );
            }
        }

        WHEN("the source that only the Tupfile directory's copy includes is edited")
        {
            f.write_file("decoy.h", "#define D 2\n");
            auto const result = f.build();

            THEN("the rule does not rebuild, because it never compiled that file")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Nothing to do") != std::string::npos);
            }
        }
    }
}

SCENARIO("An object no scanned invocation writes is reported beside its scanned sibling", "[e2e][strict][depscan]")
{
    GIVEN("a rule whose compile prefix covers one declared object and whose tail covers the other")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("a.c", "#include \"a.h\"\nint a(void){return 0;}\n");
        f.write_file("a.h", "#define A 1\n");
        f.write_file("Tupfile", ": a.c |> gcc -c a.c -o a.o && cp a.o b.o |> a.o b.o\n");

        WHEN("parse reports on the rule")
        {
            auto result = f.pup({ "parse" });

            THEN("the copied object is named and the compiled one is not")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("no dependency scan") != std::string::npos);
                REQUIRE(result.stderr_output.find("'b.o'") != std::string::npos);
                REQUIRE(result.stderr_output.find("'a.o'") == std::string::npos);
            }
        }
    }

    GIVEN("a scanned rule in a subdirectory whose output word points into a variant directory")
    {
        // %o one directory down expands to '../../build/src/lib/a.o' -- the word resolves against
        // the rule's own directory and back into the variant, which a root-level rule never shows.
        auto f = E2EFixture { "variant_config_input" };
        f.mkdir("src/lib");
        f.write_file("src/lib/a.c", "#include \"a.h\"\nint a(void){return 0;}\n");
        f.write_file("src/lib/a.h", "#define A 1\n");
        f.write_file("src/lib/Tupfile", ": foreach *.c |> gcc -c %f -o %o |> %B.o\n");
        f.mkdir("build");
        f.write_file("build/tup.config", "");

        WHEN("parse reports on the rule")
        {
            auto result = f.pup({ "parse", "-B", "build" });

            THEN("the scanned object is not named")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("no dependency scan") == std::string::npos);
            }
        }
    }

    GIVEN("a single-invocation rule declaring an object its compile never writes")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("a.c", "int a(void){return 0;}\n");
        f.write_file("Tupfile", ": a.c |> gcc -c a.c -o a.o |> a.o b.o\n");

        WHEN("parse reports on the rule")
        {
            auto result = f.pup({ "parse" });

            THEN("the object attributable to no compile is named even though a scan exists")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.stderr_output.find("'b.o'") != std::string::npos);
                REQUIRE(result.stderr_output.find("'a.o'") == std::string::npos);
            }
        }
    }
}

SCENARIO("A compile-shaped rule with no dependency scan is reported", "[e2e][strict][depscan]")
{
    // The scan is declined correctly — putup cannot reproduce the prefix's shell state — but
    // declining in silence leaves a rule whose headers are never recorded (#352).
    GIVEN("a project with a scanned compile, an announced compile, an unscanned compile, and a self-depfiling compile")
    {
        auto f = E2EFixture { "unscanned_compile" };
        REQUIRE(f.init().success());

        WHEN("parse reports at the default check level")
        {
            auto result = f.pup({ "parse" });

            THEN("the unscanned rule is named and the other three are not")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("no dependency scan") != std::string::npos);
                REQUIRE(result.stderr_output.find("hidden.o") != std::string::npos);
                REQUIRE(result.stderr_output.find("plain.o") == std::string::npos);
                REQUIRE(result.stderr_output.find("shown.o") == std::string::npos);
                REQUIRE(result.stderr_output.find("owndep.o") == std::string::npos);
            }
        }

        WHEN("parse --check=none is run")
        {
            auto result = f.pup({ "parse", "--check=none" });

            THEN("nothing is reported")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("no dependency scan") == std::string::npos);
            }
        }

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the build summarizes the count once without naming rules")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("1 object file has no dependency scan") != std::string::npos);
                REQUIRE(result.stdout_output.find("hidden.o") == std::string::npos);
            }

            THEN("the command it names is one putup accepts")
            {
                auto listed = f.pup({ "parse" });
                INFO("stderr: " << listed.stderr_output);
                REQUIRE(result.stdout_output.find("run 'putup parse' for the list") != std::string::npos);
                REQUIRE(listed.success());
                REQUIRE(listed.stderr_output.find("hidden.o") != std::string::npos);
            }
        }
    }
}

SCENARIO("A depfile flag the compile never carried hides no unscanned object", "[e2e][strict][depscan]")
{
    // The suppression exists for a compile that writes its own depfile; read across the whole
    // command text it was defeated by any word spelling one, including in a later invocation (#357).
    GIVEN("a rule that spells a depfile flag after the compile it could not scan")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("a.c", "#include \"a.h\"\nint a(void){return 0;}\n");
        f.write_file("a.h", "#define A 1\n");
        f.write_file("Tupfile", ": a.c |> cd . && gcc -c a.c -o a.o && echo -MD |> a.o\n");

        WHEN("parse reports on the rule")
        {
            auto result = f.pup({ "parse" });

            THEN("the object is still named")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("no dependency scan") != std::string::npos);
                REQUIRE(result.stderr_output.find("a.o") != std::string::npos);
            }
        }
    }

    GIVEN("a rule whose announcement carries the flag its compile does not")
    {
        auto f = E2EFixture { "glob_mixed_space" };
        f.write_file("a.c", "#include \"a.h\"\nint a(void){return 0;}\n");
        f.write_file("a.h", "#define A 1\n");
        f.write_file("Tupfile", ": a.c |> echo -MD && gcc -c a.c -o a.o && cp a.o b.o |> a.o b.o\n");

        WHEN("parse reports on the rule")
        {
            auto result = f.pup({ "parse" });

            THEN("the object no scan covers is still named")
            {
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(result.stderr_output.find("no dependency scan") != std::string::npos);
                REQUIRE(result.stderr_output.find("b.o") != std::string::npos);
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
                // The fixture's display is "AR %o", so the word after it is the rendered %o: a bare "libmath.a" is the bug, meaning the archive was written to the source tree.
                auto ar_pos = result.stdout_output.find("AR ");
                REQUIRE(ar_pos != std::string::npos);
                auto output_start = ar_pos + 3; // strlen("AR ")
                // Bounded to the line: %o is the last word on it, and the next line begins "[build]", which would satisfy the check below on its own.
                auto output_end = result.stdout_output.find_first_of(" \n", output_start);
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
                REQUIRE(result.stdout_output.find("Build completed: 3 commands") != std::string::npos);
            }
        }

        WHEN("--rerun is scoped to one directory")
        {
            auto result = f.build({ "-B", "build", "--rerun", "lib" });

            THEN("only that directory's commands execute")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("Build completed: 2 commands") != std::string::npos);
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

            THEN("the build does not report success")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
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

            THEN("the build does not report success")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
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

            THEN("the build does not report success")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
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

            THEN("the build does not report success")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
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

            THEN("the build does not report success")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
            }
        }
    }
}

SCENARIO("A keep-going build that refused a directory does not report it up to date", "[e2e][incremental][keep-going]")
{
    GIVEN("a fully-built two-directory project whose beta stops evaluating")
    {
        auto f = E2EFixture { "scoped_stale" };
        f.write_file("alpha/a.c", "int a(void) { return 1; }\n");
        f.write_file("alpha/Tupfile", ": a.c |> cp %f %o |> a.out\n");
        f.write_file("beta/b.c", "int b(void) { return 2; }\n");
        f.write_file("beta/Tupfile", ": b.c |> cp %f %o |> b.out\n");
        f.write_file("build/tup.config", "");
        REQUIRE(f.pup({ "configure", "-B", "build" }).success());
        REQUIRE(f.build({ "-B", "build" }).success());
        f.write_file("beta/Tupfile", "include missing.tup\n: b.c |> cp %f %o |> b.out\n");

        WHEN("a keep-going build runs")
        {
            auto result = f.build({ "-B", "build", "-k" });

            THEN("it fails rather than reporting there was nothing to do")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("Nothing to do") == std::string::npos);
            }

            AND_WHEN("a second keep-going build runs with nothing changed")
            {
                auto again = f.build({ "-B", "build", "-k" });

                THEN("it fails too, rather than reporting the tree up to date")
                {
                    INFO("stdout: " << again.stdout_output);
                    INFO("stderr: " << again.stderr_output);
                    REQUIRE_FALSE(again.success());
                    REQUIRE(again.stdout_output.find("up to date") == std::string::npos);
                }
            }
        }

        WHEN("the directory that stopped evaluating is excluded")
        {
            auto result = f.build({ "-B", "build", "-k", "-x", "beta" });

            THEN("the build succeeds, because nothing refused a directory this build asked for")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }
        }

        WHEN("alpha asks for beta through a group and the build is scoped to alpha")
        {
            f.write_file("alpha/Tupfile", ": a.c | ../beta/<grp> |> cp %f %o |> a.out\n");
            f.write_file("beta/Tupfile", "error broken\n: b.c |> cp %f %o |> b.out <grp>\n");
            auto result = f.build({ "-B", "build", "-k", "alpha" });

            THEN("it fails, because the group reference asked for beta")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
            }

            AND_WHEN("beta is excluded rather than the build being scoped")
            {
                auto excluded = f.build({ "-B", "build", "-k", "-x", "beta" });

                THEN("it fails too, since excluding beta does not unask for its group")
                {
                    INFO("stdout: " << excluded.stdout_output);
                    INFO("stderr: " << excluded.stderr_output);
                    REQUIRE_FALSE(excluded.success());
                }
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

            THEN("the build fails")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
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
                        REQUIRE(full.stdout_output.find("Build completed: 1 commands") != std::string::npos);
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
                REQUIRE(full.stdout_output.find("Build completed: 2 commands") != std::string::npos);
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

SCENARIO("A continuation without a space before it builds and is scanned", "[e2e][incremental]")
{
    // Upstream tup rewrites `\`+newline to spaces, so the command runs as `gcc -c foo.c`;
    // keeping the two bytes made the shell splice them into `-cfoo.c` and hid the compile
    // from the dep scanner.
    auto env = EnvGuard { "PUP_IMPLICIT_DEPS", "1" };

    GIVEN("a rule whose continuation carries no space before the backslash")
    {
        auto f = E2EFixture { "continuation_glued" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the build succeeds")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
            }
        }

        WHEN("a header the compile includes changes")
        {
            REQUIRE(f.build().success());
            REQUIRE(f.build().is_noop());
            f.write_file("value.h", "#define VALUE 2\n");
            auto result = f.build();

            THEN("the compile reruns, so the scan recorded the header")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
            }
        }
    }
}

SCENARIO("compdb reports no argument carrying a newline", "[e2e][show]")
{
    GIVEN("a rule spread over three lines by continuations")
    {
        auto f = E2EFixture { "continuation_spaced" };
        REQUIRE(f.init().success());

        WHEN("show compdb is run")
        {
            auto result = f.pup({ "show", "compdb" });

            THEN("no argument is or contains a newline")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(result.stdout_output.find("\"-I.\", \"foo.c\"") != std::string::npos);
                REQUIRE(result.stdout_output.find("\\n") == std::string::npos);
            }
        }
    }
}

SCENARIO("A CRLF Tupfile builds what its LF twin builds", "[e2e][build]")
{
    GIVEN("a project whose assignment line ends in CRLF")
    {
        auto f = E2EFixture { "crlf_conditional" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the conditional's rule runs")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE_FALSE(result.is_noop());
                REQUIRE(f.exists("foo.o"));
            }
        }
    }
}

SCENARIO("A CRLF Tupfile names its outputs without the carriage return", "[e2e][build]")
{
    GIVEN("a rule whose output line ends in CRLF")
    {
        auto f = E2EFixture { "crlf_output_name" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the output is named as written")
            {
                INFO("stdout: " << result.stdout_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("foo.o"));
            }

            THEN("no file carries a carriage return in its name")
            {
                REQUIRE(result.success());
                for (auto const& entry : std::filesystem::recursive_directory_iterator { f.workdir() }) {
                    auto const name = entry.path().filename().string();
                    INFO("entry: " << entry.path().string());
                    REQUIRE(name.find('\r') == std::string::npos);
                }
            }
        }
    }
}

SCENARIO("A CRLF include line resolves", "[e2e][build]")
{
    GIVEN("a Tupfile whose include line ends in CRLF")
    {
        auto f = E2EFixture { "crlf_include" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the included file is found and its value used")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("foo.o") == "[1]\n");
            }
        }
    }
}

SCENARIO("A command that creates none of its declared outputs names every one of them", "[e2e][build]")
{
    GIVEN("a rule declaring two outputs whose command exits zero writing neither")
    {
        auto f = E2EFixture { "missing_declared_output" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the build fails")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
            }

            THEN("both outputs that were never written are named")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("expected to write to file 'a.out'") != std::string::npos);
                REQUIRE(combined.find("expected to write to file 'b.out'") != std::string::npos);
            }
        }
    }
}

SCENARIO("A command that writes only some of its declared outputs fails the build", "[e2e][build]")
{
    GIVEN("a rule declaring two outputs whose command writes the first")
    {
        auto f = E2EFixture { "partially_written_outputs" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the build fails naming only the output that was never written")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("expected to write to file 'b.out'") != std::string::npos);
                REQUIRE(combined.find("expected to write to file 'a.out'") == std::string::npos);
            }
        }
    }
}

SCENARIO("An extra output is left out of %o", "[e2e][build]")
{
    GIVEN("a rule with one regular output and one extra output")
    {
        auto f = E2EFixture { "extra_output_excluded_from_percent_o" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("%o names the regular output and not the extra one")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("side.log"));
                auto const expanded = f.read_file("out.txt");
                INFO("%o expanded to: " << expanded);
                REQUIRE(expanded.find("out.txt") != std::string::npos);
                REQUIRE(expanded.find("side.log") == std::string::npos);
            }
        }
    }
}

SCENARIO("An extra output is owned by its command and removed by clean", "[e2e][build]")
{
    GIVEN("a rule with one regular output and one extra output")
    {
        auto f = E2EFixture { "extra_output_is_tracked" };
        REQUIRE(f.init().success());

        WHEN("the project is built and then cleaned")
        {
            auto build_result = f.build();
            INFO("stdout: " << build_result.stdout_output);
            INFO("stderr: " << build_result.stderr_output);
            REQUIRE(build_result.success());
            REQUIRE(f.exists("side.log"));

            auto clean_result = f.clean();

            THEN("the extra output is removed alongside the regular one")
            {
                INFO("stdout: " << clean_result.stdout_output);
                INFO("stderr: " << clean_result.stderr_output);
                REQUIRE(clean_result.success());
                REQUIRE_FALSE(f.exists("out.txt"));
                REQUIRE_FALSE(f.exists("side.log"));
            }
        }
    }
}

SCENARIO("A bang macro's extra outputs join the rule's own rather than replacing them", "[e2e][build]")
{
    GIVEN("a bang macro declaring an extra output used by a rule declaring another")
    {
        auto f = E2EFixture { "extra_output_macro_and_rule_union" };
        REQUIRE(f.init().success());

        WHEN("the project is built and then cleaned")
        {
            auto build_result = f.build();
            INFO("stdout: " << build_result.stdout_output);
            INFO("stderr: " << build_result.stderr_output);
            REQUIRE(build_result.success());

            auto clean_result = f.clean();

            THEN("both extra outputs are owned and removed")
            {
                INFO("stdout: " << clean_result.stdout_output);
                INFO("stderr: " << clean_result.stderr_output);
                REQUIRE(clean_result.success());
                REQUIRE_FALSE(f.exists("macro.log"));
                REQUIRE_FALSE(f.exists("rule.log"));
            }
        }
    }
}

SCENARIO("A percent-O extra output names the primary output without its extension", "[e2e][build]")
{
    GIVEN("a rule whose extra output is spelled with %O, upstream's linker-map idiom")
    {
        auto f = E2EFixture { "percent_o_ldmap" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the extra output keeps the primary output's directory and drops its extension")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("sub/out.map"));
            }
        }
    }
}

SCENARIO("A percent-O in the outputs section is refused", "[e2e][build]")
{
    GIVEN("a rule whose primary output is spelled with %O")
    {
        auto f = E2EFixture { "percent_o_in_primary_outputs" };

        WHEN("the project is configured")
        {
            auto result = f.init();

            THEN("the Tupfile is rejected rather than naming an output after itself")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("%O can only be used in the extra outputs section") != std::string::npos);
            }
        }
    }
}

SCENARIO("A percent-O in a command string names the output without its extension", "[e2e][build]")
{
    GIVEN("a rule whose command spells %O")
    {
        auto f = E2EFixture { "percent_o_in_command" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the command writes the file %O names")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("out.map"));
            }
        }
    }
}

SCENARIO("A percent-O extra output keeps the directory the output was declared with", "[e2e][build]")
{
    GIVEN("a rule in a subdirectory whose output is nested and whose extra output carries a prefix")
    {
        auto f = E2EFixture { "percent_o_subdir_declared" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the extra output is named from the declared path, not the path it resolves to")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("d/sub/out/file.txt.2"));
                REQUIRE_FALSE(f.exists("d/sub/d/out/file.txt.2"));
            }
        }
    }
}

SCENARIO("A percent-O with more than one output is refused", "[e2e][build]")
{
    GIVEN("a rule declaring two outputs and an extra output spelled with %O")
    {
        auto f = E2EFixture { "percent_o_multiple_outputs" };

        WHEN("the project is configured")
        {
            auto result = f.init();

            THEN("the Tupfile is rejected rather than silently taking the first output")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("exactly one output") != std::string::npos);
            }
        }
    }
}

SCENARIO("A percent-O on an output with no extension keeps the whole name", "[e2e][build]")
{
    GIVEN("a rule whose only output has no extension")
    {
        auto f = E2EFixture { "percent_o_extensionless" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the extra output is named from the whole output rather than from nothing")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("foo.map"));
            }
        }
    }
}

SCENARIO("A percent-o extra output names the primary outputs", "[e2e][build]")
{
    GIVEN("a rule whose extra output is spelled with %o")
    {
        auto f = E2EFixture { "percent_lower_o_in_extras" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the extra output is named from the primary output")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("out.txt.log"));
            }
        }
    }
}

SCENARIO("A percent-o in the outputs section is refused", "[e2e][build]")
{
    GIVEN("a rule whose primary output is spelled with %o")
    {
        auto f = E2EFixture { "percent_lower_o_in_primary" };

        WHEN("the project is configured")
        {
            auto result = f.init();

            THEN("the Tupfile is rejected rather than declaring an output named from nothing")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("%o can only be used in a command string or extra outputs section") != std::string::npos);
            }
        }
    }
}

SCENARIO("A group directory prefix after an extra outputs section names the group", "[e2e][build][groups]")
{
    GIVEN("a rule whose extra outputs section ends in a path prefix for an order-only group")
    {
        auto f = E2EFixture { "extra_output_group_dir_prefix" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the prefix is the group's directory rather than a declared output")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("side.log"));
                REQUIRE_FALSE(f.exists("sub"));
                REQUIRE(f.exists("two.txt"));
                // An unresolved group only warns, so the silence is what witnesses the prefix moved it.
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(combined.find("has no members") == std::string::npos);
            }
        }
    }
}

SCENARIO("A carried-forward record notices its extra output changed", "[e2e][build][incremental]")
{
    GIVEN("a rule with an extra output consumed by a rule in another directory")
    {
        auto f = E2EFixture { "extra_output_merge_rerun" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.read_file("a/side.log") == "v1\n");

        WHEN("the extra output is changed behind the build and only the consumer is in scope")
        {
            auto before = f.pup({ "show", "index" });
            REQUIRE(before.stdout_output.find("[a]  must_rerun") == std::string::npos);

            f.write_file("a/side.log", "corrupted\n");
            auto scoped = f.build({ "b" });
            INFO("stdout: " << scoped.stdout_output);
            INFO("stderr: " << scoped.stderr_output);
            REQUIRE(scoped.success());

            THEN("the out-of-scope producer is carried forward marked for rerun")
            {
                auto index = f.pup({ "show", "index" });
                INFO("index: " << index.stdout_output);
                REQUIRE(index.stdout_output.find("[a]  must_rerun") != std::string::npos);
            }
        }
    }
}

SCENARIO("Declaring another output re-runs the command that writes it", "[e2e][build][incremental]")
{
    GIVEN("a built rule whose command writes a file it does not declare")
    {
        auto f = E2EFixture { "declared_output_set_identity" };
        REQUIRE(f.init().success());
        REQUIRE(f.build().success());
        REQUIRE(f.exists("second.txt"));

        WHEN("the file is added to the rule's outputs without changing the command text")
        {
            f.write_file("Tupfile", ": |> sh -c 'echo hi > %1o; echo two > second.txt' |> out.txt second.txt\n");
            f.remove_file("second.txt");
            auto result = f.build();

            THEN("the command runs and produces the newly declared output")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("second.txt"));
            }
        }

        WHEN("the file is added as an extra output, which no command text can name")
        {
            f.write_file("Tupfile", ": |> sh -c 'echo hi > %1o; echo two > second.txt' |> out.txt | second.txt\n");
            f.remove_file("second.txt");
            auto result = f.build();

            THEN("the command runs and produces it on the very next build")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.exists("second.txt"));
            }
        }
    }
}

SCENARIO("A percent-o in a rule with no outputs is refused", "[e2e][build]")
{
    GIVEN("a rule whose command spells %o but which declares no outputs")
    {
        auto f = E2EFixture { "percent_o_no_outputs" };

        WHEN("the project is configured")
        {
            auto result = f.init();

            THEN("the Tupfile is rejected rather than running a command with the flag dropped")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(
                    combined.find("%o used in rule pattern and no output files were specified")
                    != std::string::npos
                );
            }
        }
    }
}

SCENARIO("A percent-O in a command string with more than one output is refused", "[e2e][build]")
{
    GIVEN("a rule whose command spells %O while the rule declares two outputs")
    {
        auto f = E2EFixture { "percent_o_arity_in_command" };

        WHEN("the project is configured")
        {
            auto result = f.init();

            THEN("the arity rule applies outside an extra outputs section too")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE_FALSE(result.success());
                auto const combined = result.stdout_output + result.stderr_output;
                REQUIRE(
                    combined.find("%O can only be used if there is exactly one output specified")
                    != std::string::npos
                );
            }
        }
    }
}

SCENARIO("Numbered input flags name the basename and the basename without extension", "[e2e][build]")
{
    GIVEN("a rule whose input sits in a subdirectory and whose command spells %1f, %1b and %1B")
    {
        auto f = E2EFixture { "numbered_flag_basenames" };

        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("each flag expands to its own spelling of the input")
            {
                INFO("stdout: " << result.stdout_output);
                INFO("stderr: " << result.stderr_output);
                REQUIRE(result.success());
                REQUIRE(f.read_file("out.txt") == "F=src/foo.c B=foo.c BB=foo\n");
            }
        }
    }
}
