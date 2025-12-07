// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/exec/runner.hpp"
#include "pup/exec/scheduler.hpp"

#include <chrono>
#include <thread>

using namespace pup;
using namespace pup::exec;

TEST_CASE("CommandRunner basic execution", "[exec]")
{
    auto runner = CommandRunner{};

    SECTION("echo command")
    {
        auto result = runner.run("echo hello");
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code == 0);
        REQUIRE(result->stdout_output == "hello\n");
    }

    SECTION("false command returns non-zero")
    {
        auto result = runner.run("false");
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code != 0);
    }

    SECTION("true command returns zero")
    {
        auto result = runner.run("true");
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code == 0);
    }

    SECTION("capture stderr")
    {
        auto result = runner.run("echo error >&2");
        REQUIRE(result.has_value());
        REQUIRE(result->stderr_output == "error\n");
    }

    SECTION("working directory")
    {
        auto opts = RunOptions{.working_dir = "/tmp"};
        auto result = runner.run("pwd", opts);
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code == 0);
        // May have trailing newline and/or resolve to /private/tmp on macOS
        REQUIRE(result->stdout_output.find("tmp") != std::string::npos);
    }

    SECTION("environment variable")
    {
        auto opts = RunOptions{
            .env = {"MY_TEST_VAR=hello123"},
            .inherit_env = true,
        };
        auto result = runner.run("echo $MY_TEST_VAR", opts);
        REQUIRE(result.has_value());
        REQUIRE(result->stdout_output == "hello123\n");
    }
}

TEST_CASE("CommandRunner timeout", "[exec]")
{
    auto runner = CommandRunner{};

    SECTION("command completes before timeout")
    {
        auto opts = RunOptions{
            .timeout = std::chrono::seconds{5},
        };
        auto result = runner.run("echo fast", opts);
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
    }

    SECTION("command times out")
    {
        auto opts = RunOptions{
            .timeout = std::chrono::seconds{1},
        };
        auto result = runner.run("sleep 10", opts);
        REQUIRE(result.has_value());
        REQUIRE(result->timed_out);
    }
}

TEST_CASE("CommandRunner with callback", "[exec]")
{
    auto runner = CommandRunner{};
    auto output_received = std::string{};
    auto stderr_received = std::string{};

    auto callback = [&](std::string_view data, bool is_stderr) {
        if (is_stderr)
            stderr_received.append(data);
        else
            output_received.append(data);
    };

    SECTION("stdout callback")
    {
        auto result = runner.run_with_output("echo callback_test", callback);
        REQUIRE(result.has_value());
        REQUIRE(output_received == "callback_test\n");
    }

    SECTION("stderr callback")
    {
        auto result = runner.run_with_output("echo stderr_test >&2", callback);
        REQUIRE(result.has_value());
        REQUIRE(stderr_received == "stderr_test\n");
    }
}

TEST_CASE("parse_command", "[exec]")
{
    SECTION("simple words")
    {
        auto args = parse_command("echo hello world");
        REQUIRE(args.size() == 3);
        REQUIRE(args[0] == "echo");
        REQUIRE(args[1] == "hello");
        REQUIRE(args[2] == "world");
    }

    SECTION("single quotes")
    {
        auto args = parse_command("echo 'hello world'");
        REQUIRE(args.size() == 2);
        REQUIRE(args[0] == "echo");
        REQUIRE(args[1] == "hello world");
    }

    SECTION("double quotes")
    {
        auto args = parse_command("echo \"hello world\"");
        REQUIRE(args.size() == 2);
        REQUIRE(args[0] == "echo");
        REQUIRE(args[1] == "hello world");
    }

    SECTION("escaped spaces")
    {
        auto args = parse_command("echo hello\\ world");
        REQUIRE(args.size() == 2);
        REQUIRE(args[0] == "echo");
        REQUIRE(args[1] == "hello world");
    }

    SECTION("empty input")
    {
        auto args = parse_command("");
        REQUIRE(args.empty());
    }

    SECTION("only whitespace")
    {
        auto args = parse_command("   ");
        REQUIRE(args.empty());
    }
}

TEST_CASE("shell_quote", "[exec]")
{
    SECTION("simple string needs no quoting")
    {
        REQUIRE(shell_quote("hello") == "hello");
        REQUIRE(shell_quote("file.txt") == "file.txt");
        REQUIRE(shell_quote("/path/to/file") == "/path/to/file");
    }

    SECTION("string with spaces")
    {
        REQUIRE(shell_quote("hello world") == "'hello world'");
    }

    SECTION("string with single quote")
    {
        REQUIRE(shell_quote("it's") == "'it'\"'\"'s'");
    }

    SECTION("string with special chars")
    {
        REQUIRE(shell_quote("foo$bar") == "'foo$bar'");
        REQUIRE(shell_quote("a;b") == "'a;b'");
    }
}

TEST_CASE("detect_parallelism", "[exec]")
{
    auto parallelism = detect_parallelism();
    REQUIRE(parallelism >= 1);
}

TEST_CASE("Scheduler basic operation", "[exec]")
{
    SECTION("empty graph")
    {
        auto graph = graph::BuildGraph{};
        auto scheduler = Scheduler{};
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 0);
        REQUIRE(result->completed_jobs == 0);
    }

    SECTION("single command dry run")
    {
        auto graph = graph::BuildGraph{};

        auto input_id = graph.add_node(graph::Node{
            .type = NodeType::File,
            .path = "input.txt",
        });

        auto cmd_id = graph.add_node(graph::Node{
            .type = NodeType::Command,
            .command = "cat input.txt > output.txt",
            .display = "CAT input.txt",
        });

        auto output_id = graph.add_node(graph::Node{
            .type = NodeType::Generated,
            .path = "output.txt",
        });

        (void)graph.add_edge(*input_id, *cmd_id);
        (void)graph.add_edge(*cmd_id, *output_id);

        auto opts = SchedulerOptions{
            .jobs = 1,
            .dry_run = true,
        };

        auto scheduler = Scheduler{opts};
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 1);
        REQUIRE(result->completed_jobs == 1);
    }
}
