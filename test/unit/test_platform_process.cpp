// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"
#include "temp_root.hpp"

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/process.hpp"

#include <cctype>
#include <filesystem>
#include <system_error>

using namespace pup::platform;
using pup::StringId;
using pup::global_pool;
using pup::test::EnvGuard;

namespace {

auto sv(StringId id) -> std::string_view { return global_pool().get(id); }
auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }

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

auto make_opts(std::string_view cmd) -> ProcessOptions
{
    auto opts = ProcessOptions {};
    opts.command = intern(cmd);
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
                REQUIRE(sv(result->stdout_output).find("hello") != std::string::npos);
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
        // A plain temp directory is all this needs — no E2EFixture (which
        // requires a putup binary and aborts on runners that lack one).
        auto workdir = pup::test::temp_dir("pup_run_process_wd") / "subdir";
        auto ec = std::error_code {};
        std::filesystem::create_directories(workdir, ec);

        auto opts = make_opts(PWD_CMD);
        opts.working_dir = intern(workdir.string());
        opts.capture_stdout = true;

        WHEN("the process is executed")
        {
            auto result = run_process(opts);

            THEN("output shows the specified directory")
            {
                REQUIRE(result.has_value());
                REQUIRE(sv(result->stdout_output).find("subdir") != std::string::npos);
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
        opts.stdin_data = intern("hello from stdin");

        WHEN("stdin data is provided")
        {
            auto result = run_process(opts);

            THEN("the data appears in stdout")
            {
                REQUIRE(result.has_value());
                REQUIRE(sv(result->stdout_output).find("hello from stdin") != std::string::npos);
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
                REQUIRE(sv(result->stderr_output).find("error message") != std::string::npos);
            }

            THEN("stdout is empty or separate from stderr")
            {
                REQUIRE(sv(result->stdout_output).find("error message") == std::string::npos);
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
        opts.env = { intern("PUP_TEST_VAR=test_value_123") };
        opts.inherit_env = true;
        opts.capture_stdout = true;

        WHEN("the process is executed with custom env")
        {
            auto result = run_process(opts);

            THEN("the environment variable is visible")
            {
                REQUIRE(result.has_value());
                REQUIRE(sv(result->stdout_output).find("test_value_123") != std::string::npos);
            }
        }
    }
}

TEST_CASE("build_env_strings constructs environment list", "[platform][process]")
{
    SECTION("with inherit_env=false returns only extra vars")
    {
        auto extra = pup::Vec<pup::StringId> { intern("FOO=bar"), intern("BAZ=qux") };
        auto result = build_env_strings(extra, false);

        REQUIRE(result.size() == 2);
        REQUIRE(sv(result[0]) == "FOO=bar");
        REQUIRE(sv(result[1]) == "BAZ=qux");
    }

    SECTION("with inherit_env=true includes current environment")
    {
        auto extra = pup::Vec<pup::StringId> { intern("EXTRA=value") };
        auto result = build_env_strings(extra, true);

        REQUIRE(result.size() > 1);

        auto has_extra = false;
        for (auto const& var : result) {
            if (sv(var) == "EXTRA=value") {
                has_extra = true;
                break;
            }
        }
        REQUIRE(has_extra);
    }
}

TEST_CASE("base_child_env forwards the temporary-directory variables the tools read", "[platform][process]")
{
    auto has_name = [](pup::Vec<pup::StringId> const& env, std::string_view name) {
        for (auto var : env) {
            auto entry = sv(var);
            if (entry.find('=') != name.size()) {
                continue;
            }
            auto same = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
#ifdef _WIN32
                same = std::toupper(static_cast<unsigned char>(entry[i])) == name[i];
#else
                same = entry[i] == name[i];
#endif
                if (!same) {
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
        return false;
    };

#ifdef _WIN32
    SECTION("the Windows keep list carries TEMP and TMP and not TMPDIR")
    {
        auto tmpdir = EnvGuard { "TMPDIR", "C:\\pup-probe\\tmpdir" };
        auto env = base_child_env();

        REQUIRE(has_name(env, "TEMP"));
        REQUIRE(has_name(env, "TMP"));
        REQUIRE_FALSE(has_name(env, "TMPDIR"));
    }
#else
    auto contains = [](pup::Vec<pup::StringId> const& env, std::string_view entry) {
        for (auto var : env) {
            if (sv(var) == entry) {
                return true;
            }
        }
        return false;
    };

    SECTION("TMPDIR, TMP and TEMP reach the child when set")
    {
        auto tmpdir = EnvGuard { "TMPDIR", "/pup-probe/tmpdir" };
        auto tmp = EnvGuard { "TMP", "/pup-probe/tmp" };
        auto temp = EnvGuard { "TEMP", "/pup-probe/temp" };
        auto env = base_child_env();

        REQUIRE(contains(env, "TMPDIR=/pup-probe/tmpdir"));
        REQUIRE(contains(env, "TMP=/pup-probe/tmp"));
        REQUIRE(contains(env, "TEMP=/pup-probe/temp"));
        REQUIRE(has_name(env, "PATH"));
    }

    SECTION("an unset TMPDIR is absent rather than empty")
    {
        auto restore = EnvGuard { "TMPDIR", "/pup-probe/tmpdir" };
        pup::platform::unset_env("TMPDIR");
        auto env = base_child_env();

        REQUIRE_FALSE(has_name(env, "TMPDIR"));
        REQUIRE(has_name(env, "PATH"));
    }

    SECTION("a set-but-empty TMPDIR is absent rather than empty")
    {
        auto empty = EnvGuard { "TMPDIR", "" };
        auto env = base_child_env();

        REQUIRE_FALSE(has_name(env, "TMPDIR"));
        REQUIRE(has_name(env, "PATH"));
    }
#endif
}

TEST_CASE("base_child_env always gives the child a PATH and substitutes a default when putup has none", "[platform][process]")
{
#ifdef _WIN32
    SECTION("the Windows keep list carries PATH")
    {
        auto path = EnvGuard { "PATH", "C:\\pup-probe\\bin" };
        auto env = base_child_env();

        auto has_path = false;
        for (auto var : env) {
            has_path = has_path || sv(var).starts_with("PATH=");
        }
        REQUIRE(has_path);
    }
#else
    auto contains = [](pup::Vec<pup::StringId> const& env, std::string_view entry) {
        for (auto var : env) {
            if (sv(var) == entry) {
                return true;
            }
        }
        return false;
    };

    SECTION("the child gets putup's own PATH")
    {
        auto path = EnvGuard { "PATH", "/pup-probe/bin:/usr/bin" };
        auto env = base_child_env();

        REQUIRE(contains(env, "PATH=/pup-probe/bin:/usr/bin"));
    }

    SECTION("an unset PATH becomes the default search path rather than an empty one")
    {
        auto restore = EnvGuard { "PATH", "/pup-probe/bin" };
        pup::platform::unset_env("PATH");
        auto env = base_child_env();

        REQUIRE(contains(env, "PATH=/usr/bin:/bin"));
    }
#endif
}

TEST_CASE("base_child_env forwards HOME when set and omits it when unset or empty", "[platform][process]")
{
    auto has_name = [](pup::Vec<pup::StringId> const& env, std::string_view name) {
        for (auto var : env) {
            auto entry = sv(var);
            if (entry.find('=') != name.size()) {
                continue;
            }
            auto same = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
#ifdef _WIN32
                same = std::toupper(static_cast<unsigned char>(entry[i])) == name[i];
#else
                same = entry[i] == name[i];
#endif
                if (!same) {
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
        return false;
    };

#ifdef _WIN32
    SECTION("the Windows keep list carries HOME")
    {
        auto home = EnvGuard { "HOME", "C:\\pup-probe\\home" };
        auto env = base_child_env();

        REQUIRE(has_name(env, "HOME"));
    }
#else
    auto contains = [](pup::Vec<pup::StringId> const& env, std::string_view entry) {
        for (auto var : env) {
            if (sv(var) == entry) {
                return true;
            }
        }
        return false;
    };

    SECTION("HOME reaches the child when set")
    {
        auto home = EnvGuard { "HOME", "/pup-probe/home" };
        auto env = base_child_env();

        REQUIRE(contains(env, "HOME=/pup-probe/home"));
        REQUIRE(has_name(env, "PATH"));
    }

    SECTION("an unset HOME is absent rather than empty")
    {
        auto restore = EnvGuard { "HOME", "/pup-probe/home" };
        pup::platform::unset_env("HOME");
        auto env = base_child_env();

        REQUIRE_FALSE(has_name(env, "HOME"));
        REQUIRE(has_name(env, "PATH"));
    }

    SECTION("a set-but-empty HOME is absent rather than empty")
    {
        auto empty = EnvGuard { "HOME", "" };
        auto env = base_child_env();

        REQUIRE_FALSE(has_name(env, "HOME"));
        REQUIRE(has_name(env, "PATH"));
    }
#endif
}
