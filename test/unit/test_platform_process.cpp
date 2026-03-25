// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"
#include "pup/platform/process.hpp"

using namespace pup::platform;
using namespace pup::test;

namespace {

#ifdef _WIN32
auto const ECHO_CMD = std::string { "cmd /c echo" };
auto const FALSE_CMD = std::string { "cmd /c exit 1" };
auto const CAT_CMD = std::string { "more" };
auto const PWD_CMD = std::string { "cd" };
#else
auto const ECHO_CMD = std::string { "echo" };
auto const FALSE_CMD = std::string { "false" };
auto const CAT_CMD = std::string { "cat" };
auto const PWD_CMD = std::string { "pwd" };
#endif

auto make_opts(std::string cmd) -> ProcessOptions
{
    auto opts = ProcessOptions {};
    opts.command = std::move(cmd);
    return opts;
}

} // namespace

SCENARIO("run_process executes commands and captures output", "[platform][process]")
{
    GIVEN("a simple echo command")
    {
        auto opts = make_opts(ECHO_CMD + " hello");
        opts.capture_stdout = true;

        WHEN("the process is executed")
        {
            auto result = run_process(opts);

            THEN("execution succeeds")
            {
                REQUIRE(result.has_value());
            }

            THEN("exit code is zero")
            {
                REQUIRE(result->exit_code == 0);
            }

            THEN("stdout contains the echoed text")
            {
                REQUIRE(result->stdout_output.find("hello") != std::string::npos);
            }
        }
    }
}

SCENARIO("run_process handles failing commands", "[platform][process]")
{
    GIVEN("a command that exits with non-zero status")
    {
        auto opts = make_opts(FALSE_CMD);

        WHEN("the process is executed")
        {
            auto result = run_process(opts);

            THEN("execution succeeds (process ran)")
            {
                REQUIRE(result.has_value());
            }

            THEN("exit code is non-zero")
            {
                REQUIRE(result->exit_code != 0);
            }
        }
    }
}

SCENARIO("run_process respects working directory", "[platform][process]")
{
    GIVEN("a command that prints the working directory")
    {
        auto f = E2EFixture { "simple_c" };
        f.mkdir("subdir");

        auto opts = make_opts(PWD_CMD);
        opts.working_dir = (f.workdir() / "subdir").string();
        opts.capture_stdout = true;

        WHEN("the process is executed")
        {
            auto result = run_process(opts);

            THEN("output shows the specified directory")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->stdout_output.find("subdir") != std::string::npos);
            }
        }
    }
}

SCENARIO("run_process can pipe data to stdin", "[platform][process]")
{
    GIVEN("a command that reads stdin")
    {
        auto opts = make_opts(CAT_CMD);
        opts.capture_stdout = true;
        opts.stdin_data = "hello from stdin";

        WHEN("stdin data is provided")
        {
            auto result = run_process(opts);

            THEN("the data appears in stdout")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->stdout_output.find("hello from stdin") != std::string::npos);
            }
        }
    }
}

SCENARIO("run_process captures stderr separately", "[platform][process]")
{
    GIVEN("a command that writes to stderr")
    {
#ifdef _WIN32
        auto opts = make_opts("cmd /c echo error message 1>&2");
#else
        auto opts = make_opts("echo error message >&2");
#endif
        opts.capture_stdout = true;
        opts.capture_stderr = true;

        WHEN("the process is executed")
        {
            auto result = run_process(opts);

            THEN("stderr output is captured")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->stderr_output.find("error message") != std::string::npos);
            }

            THEN("stdout is empty or separate from stderr")
            {
                REQUIRE(result->stdout_output.find("error message") == std::string::npos);
            }
        }
    }
}

SCENARIO("run_process passes environment variables", "[platform][process]")
{
    GIVEN("a command that reads an environment variable")
    {
#ifdef _WIN32
        auto opts = make_opts("cmd /c echo %PUP_TEST_VAR%");
#else
        auto opts = make_opts("echo $PUP_TEST_VAR");
#endif
        opts.env = { "PUP_TEST_VAR=test_value_123" };
        opts.inherit_env = true;
        opts.capture_stdout = true;

        WHEN("the process is executed with custom env")
        {
            auto result = run_process(opts);

            THEN("the environment variable is visible")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->stdout_output.find("test_value_123") != std::string::npos);
            }
        }
    }
}

TEST_CASE("build_env_strings constructs environment list", "[platform][process]")
{
    SECTION("with inherit_env=false returns only extra vars")
    {
        auto extra = pup::Vec<std::string> { "FOO=bar", "BAZ=qux" };
        auto result = build_env_strings(extra, false);

        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == "FOO=bar");
        REQUIRE(result[1] == "BAZ=qux");
    }

    SECTION("with inherit_env=true includes current environment")
    {
        auto extra = pup::Vec<std::string> { "EXTRA=value" };
        auto result = build_env_strings(extra, true);

        REQUIRE(result.size() > 1);

        auto has_extra = false;
        for (auto const& var : result) {
            if (var == "EXTRA=value") {
                has_extra = true;
                break;
            }
        }
        REQUIRE(has_extra);
    }
}
