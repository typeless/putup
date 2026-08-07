// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/exec/runner.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/platform/process.hpp"

#include <cctype>
#include <string_view>
#include <utility>

namespace pup::exec {

CommandRunner::CommandRunner(RunOptions default_options)
    : default_options_(std::move(default_options))
{
}

auto CommandRunner::run(std::string_view command) -> Result<CommandResult>
{
    return run(command, default_options_);
}

auto CommandRunner::run(
    std::string_view command,
    RunOptions const& options
) -> Result<CommandResult>
{
    return run_with_output(command, nullptr, options);
}

namespace {

struct CallbackAdapter {
    OutputCallback const* callback;
};

auto platform_callback(std::string_view data, bool is_stderr, void* user_data) -> void
{
    auto* adapter = static_cast<CallbackAdapter*>(user_data);
    if (adapter->callback && *adapter->callback) {
        (*adapter->callback)(data, is_stderr);
    }
}

} // namespace

auto CommandRunner::run_with_output(
    std::string_view command,
    OutputCallback const& callback,
    RunOptions const& options
) -> Result<CommandResult>
{
    auto merged = RunOptions { merge_options(options) };

    auto platform_opts = pup::platform::ProcessOptions {};
    platform_opts.command = pup::global_pool().intern(command);
    platform_opts.working_dir = merged.working_dir;
    platform_opts.env = merged.env;
    platform_opts.inherit_env = merged.inherit_env;
    platform_opts.capture_stdout = merged.capture_stdout;
    platform_opts.capture_stderr = merged.capture_stderr;
    platform_opts.stdin_data = merged.stdin_data;
    platform_opts.timeout = merged.timeout;

    auto adapter = CallbackAdapter { &callback };
    auto platform_result = pup::platform::run_process_with_callback(
        platform_opts,
        callback ? platform_callback : nullptr,
        callback ? &adapter : nullptr
    );

    if (!platform_result) {
        return make_error<CommandResult>(platform_result.error().code, platform_result.error().message);
    }

    auto result = CommandResult {};
    result.exit_code = platform_result->exit_code;
    result.stdout_output = platform_result->stdout_output;
    result.stderr_output = platform_result->stderr_output;
    result.duration = platform_result->duration;
    result.timed_out = platform_result->timed_out;
    result.signaled = platform_result->signaled;
    result.signal = platform_result->signal;

    return result;
}

auto CommandRunner::merge_options(RunOptions const& options) const -> RunOptions
{
    auto merged = RunOptions { options };

    if (pup::is_empty(merged.working_dir)) {
        merged.working_dir = default_options_.working_dir;
    }

    // Merge environment variables
    auto env = default_options_.env;
    for (auto const& e : options.env) {
        env.push_back(e);
    }
    merged.env = std::move(env);

    if (!merged.timeout && default_options_.timeout) {
        merged.timeout = default_options_.timeout;
    }

    return merged;
}

auto shell_quote(std::string_view str) -> StringId
{
    auto& pool = pup::global_pool();
    auto needs_quoting = false;
    for (auto c : str) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.' && c != '/' && c != ':' && c != '@') {
            needs_quoting = true;
            break;
        }
    }

    if (!needs_quoting) {
        return pool.intern(str);
    }

    auto buf = Buf {};
    buf += '\'';
    for (auto c : str) {
        if (c == '\'') {
            buf += "'\"'\"'";
        } else {
            buf += c;
        }
    }
    buf += '\'';

    return buf.intern(pool);
}

} // namespace pup::exec
