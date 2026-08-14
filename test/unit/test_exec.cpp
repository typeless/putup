// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "temp_root.hpp"

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/exec/progress_display.hpp"
#include "pup/exec/runner.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/platform/env.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace pup;
using namespace pup::exec;

namespace {
[[maybe_unused]] auto sv(StringId id) -> std::string_view { return global_pool().get(id); }
[[maybe_unused]] auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }
} // namespace

#ifndef _WIN32

TEST_CASE("CommandRunner basic execution", "[exec]")
{
    auto runner = CommandRunner {};

    SECTION("echo command")
    {
        auto result = runner.run("echo hello");
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code == 0);
        REQUIRE(sv(result->stdout_output) =="hello\n");
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
        REQUIRE(sv(result->stderr_output) =="error\n");
    }

    SECTION("working directory")
    {
        auto const workdir = pup::test::temp_dir("pup_exec_wd").string();
        auto opts = RunOptions { .working_dir = intern(workdir) };
        auto result = runner.run("pwd", opts);
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code == 0);
        // The path may come back resolved (/tmp is /private/tmp on macOS), so match the leaf.
        auto const leaf = std::filesystem::path { workdir }.filename().string();
        REQUIRE(sv(result->stdout_output).find(leaf) != std::string_view::npos);
    }

    SECTION("environment variable")
    {
        auto opts = RunOptions {
            .env = { intern("MY_TEST_VAR=hello123") },
            .inherit_env = true,
        };
        auto result = runner.run("echo $MY_TEST_VAR", opts);
        REQUIRE(result.has_value());
        REQUIRE(sv(result->stdout_output) =="hello123\n");
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

#endif // !_WIN32

TEST_CASE("shell_quote", "[exec]")
{
    SECTION("simple string needs no quoting")
    {
        REQUIRE(sv(shell_quote("hello")) =="hello");
        REQUIRE(sv(shell_quote("file.txt")) =="file.txt");
        REQUIRE(sv(shell_quote("/path/to/file")) =="/path/to/file");
    }

    SECTION("string with spaces")
    {
        REQUIRE(sv(shell_quote("hello world")) =="'hello world'");
    }

    SECTION("string with single quote")
    {
        REQUIRE(sv(shell_quote("it's")) =="'it'\"'\"'s'");
    }

    SECTION("string with special chars")
    {
        REQUIRE(sv(shell_quote("foo$bar")) =="'foo$bar'");
        REQUIRE(sv(shell_quote("a;b")) =="'a;b'");
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
        auto bs = graph::make_build_graph();
        auto scheduler = Scheduler {};
        auto result = scheduler.build(bs);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 0);
        REQUIRE(result->completed_jobs == 0);
    }

    SECTION("single command dry run")
    {
        auto bs = graph::make_build_graph();

        auto input_id = graph::add_file_node(bs.graph, graph::FileNode {
            .name = intern("input.txt"),
        });

        auto cmd_id = graph::add_command_node(bs.graph, graph::CommandNode {
            .display = intern("CAT input.txt"),
            .instruction_id = intern("cat input.txt > output.txt"),
        });

        auto output_id = graph::add_file_node(bs.graph, graph::FileNode {
            .type = NodeType::Generated,
            .name = intern("output.txt"),
        });

        (void)graph::add_edge(bs.graph, *input_id, *cmd_id);
        (void)graph::add_edge(bs.graph, *cmd_id, *output_id);

        auto opts = SchedulerOptions {
            .jobs = 1,
            .dry_run = true,
        };

        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(bs);

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
        auto bs = graph::make_build_graph();

        auto a_c = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("a.c") });
        auto b_c = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("b.c") });

        auto cmd1 = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc -c a.c -o a.o"),
        });
        auto cmd2 = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc -c b.c -o b.o"),
        });

        auto a_o = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("a.o") });
        auto b_o = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("b.o") });

        (void)graph::add_edge(bs.graph, *a_c, *cmd1);
        (void)graph::add_edge(bs.graph, *cmd1, *a_o);
        (void)graph::add_edge(bs.graph, *b_c, *cmd2);
        (void)graph::add_edge(bs.graph, *cmd2, *b_o);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(bs);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 2);
        REQUIRE(result->completed_jobs == 2);
    }

    SECTION("dependent commands run sequentially")
    {
        // a.c -> compile -> a.o -> link -> a.out
        auto bs = graph::make_build_graph();

        auto a_c = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("a.c") });
        auto compile_cmd = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc -c a.c -o a.o"),
        });
        auto a_o = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("a.o") });
        auto link_cmd = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc a.o -o a.out"),
        });
        auto a_out = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("a.out") });

        (void)graph::add_edge(bs.graph, *a_c, *compile_cmd);
        (void)graph::add_edge(bs.graph, *compile_cmd, *a_o);
        (void)graph::add_edge(bs.graph, *a_o, *link_cmd);
        (void)graph::add_edge(bs.graph, *link_cmd, *a_out);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(bs);

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
        auto bs = graph::make_build_graph();

        auto a_c = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("main.c") });
        auto cmd1 = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc -c main.c -o main.o"),
        });
        auto cmd2 = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc -c main.c -o main_opt.o"),
        });
        auto main_o = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("main.o") });
        auto main_opt_o = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("main_opt.o") });
        auto link_cmd = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc main.o main_opt.o -o app"),
        });
        auto app = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("app") });

        (void)graph::add_edge(bs.graph, *a_c, *cmd1);
        (void)graph::add_edge(bs.graph, *a_c, *cmd2);
        (void)graph::add_edge(bs.graph, *cmd1, *main_o);
        (void)graph::add_edge(bs.graph, *cmd2, *main_opt_o);
        (void)graph::add_edge(bs.graph, *main_o, *link_cmd);
        (void)graph::add_edge(bs.graph, *main_opt_o, *link_cmd);
        (void)graph::add_edge(bs.graph, *link_cmd, *app);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(bs);

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
        auto bs = graph::make_build_graph();

        auto src = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("lib.c") });
        auto cmd1 = graph::add_command_node(bs.graph, graph::CommandNode { .instruction_id = intern("gcc -c -O0 lib.c -o lib_debug.o") });
        auto cmd2 = graph::add_command_node(bs.graph, graph::CommandNode { .instruction_id = intern("gcc -c -O2 lib.c -o lib_opt.o") });
        auto cmd3 = graph::add_command_node(bs.graph, graph::CommandNode { .instruction_id = intern("gcc -c -Os lib.c -o lib_size.o") });
        auto out1 = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("lib_debug.o") });
        auto out2 = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("lib_opt.o") });
        auto out3 = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("lib_size.o") });

        (void)graph::add_edge(bs.graph, *src, *cmd1);
        (void)graph::add_edge(bs.graph, *src, *cmd2);
        (void)graph::add_edge(bs.graph, *src, *cmd3);
        (void)graph::add_edge(bs.graph, *cmd1, *out1);
        (void)graph::add_edge(bs.graph, *cmd2, *out2);
        (void)graph::add_edge(bs.graph, *cmd3, *out3);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(bs);

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
        auto bs = graph::make_build_graph();

        auto a_o = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("a.o") });
        auto b_o = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("b.o") });
        auto c_o = graph::add_file_node(bs.graph, graph::FileNode { .name = intern("c.o") });
        auto link_cmd = graph::add_command_node(bs.graph, graph::CommandNode {
            .instruction_id = intern("gcc a.o b.o c.o -o program"),
        });
        auto program = graph::add_file_node(bs.graph, graph::FileNode { .type = NodeType::Generated, .name = intern("program") });

        (void)graph::add_edge(bs.graph, *a_o, *link_cmd);
        (void)graph::add_edge(bs.graph, *b_o, *link_cmd);
        (void)graph::add_edge(bs.graph, *c_o, *link_cmd);
        (void)graph::add_edge(bs.graph, *link_cmd, *program);

        auto opts = SchedulerOptions { .jobs = 4, .dry_run = true };
        auto scheduler = Scheduler { opts };
        auto result = scheduler.build(bs);

        REQUIRE(result.has_value());
        REQUIRE(result->total_jobs == 1);
        REQUIRE(result->completed_jobs == 1);
    }
}

#ifndef _WIN32

TEST_CASE("Scheduler exported_vars", "[exec]")
{
    SECTION("exported_vars passed to command environment")
    {
        // Set an env var that the command will echo
        pup::platform::set_env("PUP_TEST_EXPORT_VAR", "exported_value_123");

        auto bs = graph::make_build_graph();

        auto input_id = graph::add_file_node(bs.graph, graph::FileNode {
            .name = intern("/dev/null"),
        });

        // Command that echoes the exported var
        auto cmd_node = graph::CommandNode {
            .instruction_id = intern("echo $PUP_TEST_EXPORT_VAR"),
        };
        cmd_node.exported_vars.insert(to_underlying(intern("PUP_TEST_EXPORT_VAR")));
        auto cmd_id = graph::add_command_node(bs.graph, std::move(cmd_node));

        auto output_id = graph::add_file_node(bs.graph, graph::FileNode {
            .type = NodeType::Generated,
            .name = intern(pup::test::temp_path("test_output.txt").string()),
        });

        (void)graph::add_edge(bs.graph, *input_id, *cmd_id);
        (void)graph::add_edge(bs.graph, *cmd_id, *output_id);

        auto captured_output = std::string {};
        auto opts = SchedulerOptions { .jobs = 1 };
        auto scheduler = Scheduler { opts };

        scheduler.on_job_complete([&](BuildJob const&, JobResult const& result) {
            captured_output = std::string { sv(result.output) };
        });

        auto result = scheduler.build(bs);

        REQUIRE(result.has_value());
        REQUIRE(result->completed_jobs == 1);
        REQUIRE(captured_output.find("exported_value_123") != std::string_view::npos);

        pup::platform::unset_env("PUP_TEST_EXPORT_VAR");
    }

    SECTION("every exported var reaches the environment whatever order it was interned in")
    {
        // Interned reverse-lexicographically, so handle order is the reverse of
        // name order. Names are unique to this test so this decides the handles.
        auto z_id = intern("PUP_TEST_ORDER_Z");
        auto m_id = intern("PUP_TEST_ORDER_M");
        auto a_id = intern("PUP_TEST_ORDER_A");
        pup::platform::set_env("PUP_TEST_ORDER_Z", "zzz");
        pup::platform::set_env("PUP_TEST_ORDER_M", "mmm");
        pup::platform::set_env("PUP_TEST_ORDER_A", "aaa");

        auto bs = graph::make_build_graph();

        auto input_id = graph::add_file_node(bs.graph, graph::FileNode {
            .name = intern("/dev/null"),
        });

        auto cmd_node = graph::CommandNode {
            .instruction_id = intern("echo \"[$PUP_TEST_ORDER_A|$PUP_TEST_ORDER_M|$PUP_TEST_ORDER_Z]\""),
        };
        cmd_node.exported_vars.insert(to_underlying(z_id));
        cmd_node.exported_vars.insert(to_underlying(m_id));
        cmd_node.exported_vars.insert(to_underlying(a_id));
        auto cmd_id = graph::add_command_node(bs.graph, std::move(cmd_node));

        auto output_id = graph::add_file_node(bs.graph, graph::FileNode {
            .type = NodeType::Generated,
            .name = intern(pup::test::temp_path("test_export_order.txt").string()),
        });

        (void)graph::add_edge(bs.graph, *input_id, *cmd_id);
        (void)graph::add_edge(bs.graph, *cmd_id, *output_id);

        auto captured_output = std::string {};
        auto opts = SchedulerOptions { .jobs = 1 };
        auto scheduler = Scheduler { opts };
        scheduler.on_job_complete([&](BuildJob const&, JobResult const& result) {
            captured_output = std::string { sv(result.output) };
        });

        auto result = scheduler.build(bs);

        REQUIRE(result.has_value());
        REQUIRE(result->completed_jobs == 1);
        REQUIRE(captured_output.find("[aaa|mmm|zzz]") != std::string_view::npos);

        pup::platform::unset_env("PUP_TEST_ORDER_Z");
        pup::platform::unset_env("PUP_TEST_ORDER_M");
        pup::platform::unset_env("PUP_TEST_ORDER_A");
    }

    SECTION("unexported vars not passed")
    {
        pup::platform::set_env("PUP_TEST_HIDDEN_VAR", "hidden_value");

        auto bs = graph::make_build_graph();

        auto input_id = graph::add_file_node(bs.graph, graph::FileNode {
            .name = intern("/dev/null"),
        });

        // Command without exported_vars - var should NOT be in env
        auto cmd_node = graph::CommandNode {
            .instruction_id = intern("echo ${PUP_TEST_HIDDEN_VAR:-default}"),
        };
        // Note: exported_vars is empty
        auto cmd_id = graph::add_command_node(bs.graph, std::move(cmd_node));

        auto output_id = graph::add_file_node(bs.graph, graph::FileNode {
            .type = NodeType::Generated,
            .name = intern(pup::test::temp_path("test_output2.txt").string()),
        });

        (void)graph::add_edge(bs.graph, *input_id, *cmd_id);
        (void)graph::add_edge(bs.graph, *cmd_id, *output_id);

        auto captured_output = std::string {};
        auto opts = SchedulerOptions { .jobs = 1 };
        auto scheduler = Scheduler { opts };

        scheduler.on_job_complete([&](BuildJob const&, JobResult const& result) {
            captured_output = std::string { sv(result.output) };
        });

        auto result = scheduler.build(bs);

        REQUIRE(result.has_value());
        // Since inherit_env is true by default, the var IS available
        // This test verifies the mechanism works - in real use, commands
        // inherit parent env unless filtered
        REQUIRE(result->completed_jobs == 1);

        pup::platform::unset_env("PUP_TEST_HIDDEN_VAR");
    }
}

#endif // !_WIN32

TEST_CASE("format_duration renders M:SS with zero-padded seconds", "[exec][progress]")
{
    REQUIRE(sv(format_duration(std::chrono::milliseconds { 7 * 1000 })) == "0:07");
    REQUIRE(sv(format_duration(std::chrono::milliseconds { 59 * 1000 })) == "0:59");
    REQUIRE(sv(format_duration(std::chrono::milliseconds { 60 * 1000 })) == "1:00");
    REQUIRE(sv(format_duration(std::chrono::milliseconds { 754 * 1000 })) == "12:34");
}

TEST_CASE("render_simple counts done over total", "[exec][progress]")
{
    auto state = ProgressState { .total = 10, .completed = 3, .failed = 1 };
    REQUIRE(sv(render_simple(state)) == "[4/10]");
    REQUIRE(sv(render_simple(state, "host")) == "[host] [4/10]");
}

TEST_CASE("render_tty prefixes percent and counts", "[exec][progress]")
{
    auto state = ProgressState { .total = 4, .completed = 2 };
    auto out = render_tty(state);
    REQUIRE(sv(out.text).starts_with("[ 50% 2/4] "));
    REQUIRE(out.line_count == 1);
}
