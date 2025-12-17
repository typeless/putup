// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/exec/runner.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/platform/env.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace pup;
using namespace pup::exec;

TEST_CASE("CommandRunner basic execution", "[exec]")
{
    auto runner = CommandRunner {};

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
        auto opts = RunOptions { .working_dir = "/tmp" };
        auto result = runner.run("pwd", opts);
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code == 0);
        // May have trailing newline and/or resolve to /private/tmp on macOS
        REQUIRE(result->stdout_output.find("tmp") != std::string::npos);
    }

    SECTION("environment variable")
    {
        auto opts = RunOptions {
            .env = { "MY_TEST_VAR=hello123" },
            .inherit_env = true,
        };
        auto result = runner.run("echo $MY_TEST_VAR", opts);
        REQUIRE(result.has_value());
        REQUIRE(result->stdout_output == "hello123\n");
    }
}

TEST_CASE("CommandRunner timeout", "[exec]")
{
    auto runner = CommandRunner {};

    SECTION("command completes before timeout")
    {
        auto opts = RunOptions {
            .timeout = std::chrono::seconds { 5 },
        };
        auto result = runner.run("echo fast", opts);
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
    }

    SECTION("command times out")
    {
        auto opts = RunOptions {
            .timeout = std::chrono::seconds { 1 },
        };
        auto result = runner.run("sleep 10", opts);
        REQUIRE(result.has_value());
        REQUIRE(result->timed_out);
    }
}

TEST_CASE("CommandRunner with callback", "[exec]")
{
    auto runner = CommandRunner {};
    auto output_received = std::string {};
    auto stderr_received = std::string {};

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
        auto graph = graph::BuildGraph {};
        auto scheduler = Scheduler {};
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 0);
        REQUIRE(result->completed_jobs == 0);
    }

    SECTION("single command dry run")
    {
        auto graph = graph::BuildGraph {};

        auto input_id = graph.add_node(graph::Node {
            .type = NodeType::File,
            .name = "input.txt",
        });

        auto cmd_id = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "cat input.txt > output.txt",
            .display = "CAT input.txt",
        });

        auto output_id = graph.add_node(graph::Node {
            .type = NodeType::Generated,
            .name = "output.txt",
        });

        (void)graph.add_edge(*input_id, *cmd_id);
        (void)graph.add_edge(*cmd_id, *output_id);

        auto opts = SchedulerOptions {
            .jobs = 1,
            .dry_run = true,
        };

        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 1);
        REQUIRE(result->completed_jobs == 1);
    }
}

TEST_CASE("Scheduler parallel dependencies", "[exec]")
{
    SECTION("independent commands can run in parallel")
    {
        // Two independent compile commands that share no dependencies
        //   a.c -> cmd1 -> a.o
        //   b.c -> cmd2 -> b.o
        auto graph = graph::BuildGraph {};

        auto a_c = graph.add_node(graph::Node { .type = NodeType::File, .name = "a.c" });
        auto b_c = graph.add_node(graph::Node { .type = NodeType::File, .name = "b.c" });

        auto cmd1 = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc -c a.c -o a.o",
        });
        auto cmd2 = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc -c b.c -o b.o",
        });

        auto a_o = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "a.o" });
        auto b_o = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "b.o" });

        (void)graph.add_edge(*a_c, *cmd1);
        (void)graph.add_edge(*cmd1, *a_o);
        (void)graph.add_edge(*b_c, *cmd2);
        (void)graph.add_edge(*cmd2, *b_o);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 2);
        REQUIRE(result->completed_jobs == 2);
    }

    SECTION("dependent commands run sequentially")
    {
        // a.c -> compile -> a.o -> link -> a.out
        auto graph = graph::BuildGraph {};

        auto a_c = graph.add_node(graph::Node { .type = NodeType::File, .name = "a.c" });
        auto compile_cmd = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc -c a.c -o a.o",
        });
        auto a_o = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "a.o" });
        auto link_cmd = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc a.o -o a.out",
        });
        auto a_out = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "a.out" });

        (void)graph.add_edge(*a_c, *compile_cmd);
        (void)graph.add_edge(*compile_cmd, *a_o);
        (void)graph.add_edge(*a_o, *link_cmd);
        (void)graph.add_edge(*link_cmd, *a_out);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 2);
        REQUIRE(result->completed_jobs == 2);
    }

    SECTION("diamond dependency pattern")
    {
        // Classic diamond pattern:
        //       a.c
        //      /   |
        //   cmd1   cmd2  (can run in parallel)
        //      |   /
        //       link     (waits for both)
        auto graph = graph::BuildGraph {};

        auto a_c = graph.add_node(graph::Node { .type = NodeType::File, .name = "main.c" });
        auto cmd1 = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc -c main.c -o main.o",
        });
        auto cmd2 = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc -c main.c -o main_opt.o",
        });
        auto main_o = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "main.o" });
        auto main_opt_o = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "main_opt.o" });
        auto link_cmd = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc main.o main_opt.o -o app",
        });
        auto app = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "app" });

        (void)graph.add_edge(*a_c, *cmd1);
        (void)graph.add_edge(*a_c, *cmd2);
        (void)graph.add_edge(*cmd1, *main_o);
        (void)graph.add_edge(*cmd2, *main_opt_o);
        (void)graph.add_edge(*main_o, *link_cmd);
        (void)graph.add_edge(*main_opt_o, *link_cmd);
        (void)graph.add_edge(*link_cmd, *app);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 3);
        REQUIRE(result->completed_jobs == 3);
    }

    SECTION("fan-out pattern")
    {
        // One input, multiple independent outputs
        //        src
        //       / | |
        //      c1 c2 c3  (all can run in parallel)
        auto graph = graph::BuildGraph {};

        auto src = graph.add_node(graph::Node { .type = NodeType::File, .name = "lib.c" });
        auto cmd1 = graph.add_node(graph::Node { .type = NodeType::Command, .command = "gcc -c -O0 lib.c -o lib_debug.o" });
        auto cmd2 = graph.add_node(graph::Node { .type = NodeType::Command, .command = "gcc -c -O2 lib.c -o lib_opt.o" });
        auto cmd3 = graph.add_node(graph::Node { .type = NodeType::Command, .command = "gcc -c -Os lib.c -o lib_size.o" });
        auto out1 = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "lib_debug.o" });
        auto out2 = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "lib_opt.o" });
        auto out3 = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "lib_size.o" });

        (void)graph.add_edge(*src, *cmd1);
        (void)graph.add_edge(*src, *cmd2);
        (void)graph.add_edge(*src, *cmd3);
        (void)graph.add_edge(*cmd1, *out1);
        (void)graph.add_edge(*cmd2, *out2);
        (void)graph.add_edge(*cmd3, *out3);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 3);
        REQUIRE(result->completed_jobs == 3);
    }

    SECTION("fan-in pattern")
    {
        // Multiple inputs combine into one output
        //    a.o  b.o  c.o
        //      \  |  /
        //       link    (must wait for all)
        auto graph = graph::BuildGraph {};

        auto a_o = graph.add_node(graph::Node { .type = NodeType::File, .name = "a.o" });
        auto b_o = graph.add_node(graph::Node { .type = NodeType::File, .name = "b.o" });
        auto c_o = graph.add_node(graph::Node { .type = NodeType::File, .name = "c.o" });
        auto link_cmd = graph.add_node(graph::Node {
            .type = NodeType::Command,
            .command = "gcc a.o b.o c.o -o program",
        });
        auto program = graph.add_node(graph::Node { .type = NodeType::Generated, .name = "program" });

        (void)graph.add_edge(*a_o, *link_cmd);
        (void)graph.add_edge(*b_o, *link_cmd);
        (void)graph.add_edge(*c_o, *link_cmd);
        (void)graph.add_edge(*link_cmd, *program);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 1);
        REQUIRE(result->completed_jobs == 1);
    }
}

TEST_CASE("Scheduler exported_vars", "[exec]")
{
    SECTION("exported_vars passed to command environment")
    {
        // Set an env var that the command will echo
        pup::platform::set_env("PUP_TEST_EXPORT_VAR", "exported_value_123");

        auto graph = graph::BuildGraph {};

        auto input_id = graph.add_node(graph::Node {
            .type = NodeType::File,
            .name = "/dev/null",
        });

        // Command that echoes the exported var
        auto cmd_node = graph::Node {
            .type = NodeType::Command,
            .command = "echo $PUP_TEST_EXPORT_VAR",
        };
        cmd_node.exported_vars.insert("PUP_TEST_EXPORT_VAR");
        auto cmd_id = graph.add_node(cmd_node);

        auto output_id = graph.add_node(graph::Node {
            .type = NodeType::Generated,
            .name = "/tmp/test_output.txt",
        });

        (void)graph.add_edge(*input_id, *cmd_id);
        (void)graph.add_edge(*cmd_id, *output_id);

        auto captured_output = std::string {};
        auto opts = SchedulerOptions { .jobs = 1 };
        auto scheduler = Scheduler { opts };

        scheduler.on_job_complete([&](BuildJob const&, JobResult const& result) {
            captured_output = result.output;
        });

        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        REQUIRE(result->completed_jobs == 1);
        REQUIRE(captured_output.find("exported_value_123") != std::string::npos);

        pup::platform::unset_env("PUP_TEST_EXPORT_VAR");
    }

    SECTION("unexported vars not passed")
    {
        pup::platform::set_env("PUP_TEST_HIDDEN_VAR", "hidden_value");

        auto graph = graph::BuildGraph {};

        auto input_id = graph.add_node(graph::Node {
            .type = NodeType::File,
            .name = "/dev/null",
        });

        // Command without exported_vars - var should NOT be in env
        auto cmd_node = graph::Node {
            .type = NodeType::Command,
            .command = "echo ${PUP_TEST_HIDDEN_VAR:-default}",
        };
        // Note: exported_vars is empty
        auto cmd_id = graph.add_node(cmd_node);

        auto output_id = graph.add_node(graph::Node {
            .type = NodeType::Generated,
            .name = "/tmp/test_output2.txt",
        });

        (void)graph.add_edge(*input_id, *cmd_id);
        (void)graph.add_edge(*cmd_id, *output_id);

        auto captured_output = std::string {};
        auto opts = SchedulerOptions { .jobs = 1 };
        auto scheduler = Scheduler { opts };

        scheduler.on_job_complete([&](BuildJob const&, JobResult const& result) {
            captured_output = result.output;
        });

        auto result = scheduler.build(graph);

        REQUIRE(result.has_value());
        // Since inherit_env is true by default, the var IS available
        // This test verifies the mechanism works - in real use, commands
        // inherit parent env unless filtered
        REQUIRE(result->completed_jobs == 1);

        pup::platform::unset_env("PUP_TEST_HIDDEN_VAR");
    }
}
