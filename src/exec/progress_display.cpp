// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/exec/progress_display.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/terminal.hpp"

#include <algorithm>
#include <cstdio>

namespace pup::exec {

namespace {

auto constexpr MAX_RUNNING_JOBS_DISPLAY = std::size_t { 8 };

auto truncate_left(std::string_view str, std::size_t max_width) -> std::string_view
{
    if (str.size() <= max_width) {
        return str;
    }
    if (max_width <= 3) {
        return "...";
    }
    // For left-truncation we need to build "..." + suffix - use Buf and intern
    auto buf = Buf {};
    buf += "...";
    buf += str.substr(str.size() - (max_width - 3));
    return global_pool().get(buf.intern(global_pool()));
}

} // anonymous namespace

auto job_started(ProgressState state, NodeId id, StringId display) -> ProgressState
{
    state.running.push_back(RunningJob {
        .id = id,
        .display = display,
        .start_time = std::chrono::steady_clock::now(),
    });
    return state;
}

auto job_completed(ProgressState state, NodeId id, bool success) -> ProgressState
{
    auto& r = state.running;
    for (auto it = r.begin(); it != r.end();) {
        if (it->id == id) {
            it = r.erase(it);
        } else {
            ++it;
        }
    }
    if (success) {
        ++state.completed;
    } else {
        ++state.failed;
    }
    return state;
}

auto render_tty(ProgressState const& state, std::string_view variant) -> ProgressOutput
{
    auto& pool = global_pool();
    auto result = ProgressOutput {};
    auto out = Buf {};

    auto term_width = static_cast<std::size_t>(pup::terminal_width());

    auto sorted = state.running;
    auto now = std::chrono::steady_clock::now();
    std::ranges::sort(sorted, [now](auto const& a, auto const& b) {
        return (now - a.start_time) > (now - b.start_time);
    });

    auto done = state.completed + state.failed;
    auto pct = state.total > 0 ? (done * 100 / state.total) : std::size_t { 0 };

    auto current_display = std::string_view {};
    if (!sorted.empty()) {
        current_display = pool.get(sorted.back().display);
    }

    auto prefix = Buf {};
    if (!variant.empty()) {
        prefix += '[';
        prefix += variant;
        prefix += "] ";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%3zu%% %zu/%zu] ", pct, done, state.total);
    prefix += buf;
    auto path_width = term_width > prefix.size() ? term_width - prefix.size() : std::size_t { 20 };

    out += prefix.view();
    out += truncate_left(current_display, path_width);
    out += pup::ansi::clear_line;
    result.line_count = 1;

    auto list_size = sorted.size() > 1 ? sorted.size() - 1 : std::size_t { 0 };
    auto max_jobs = std::min(list_size, MAX_RUNNING_JOBS_DISPLAY);

    auto constexpr job_prefix_width = std::size_t { 9 };
    auto job_path_width = term_width > job_prefix_width ? term_width - job_prefix_width : std::size_t { 20 };

    for (std::size_t i = 0; i < max_jobs; ++i) {
        auto const& job = sorted[i];
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.start_time);
        out += '\n';
        out += "    ";
        out += pool.get(format_duration(elapsed));
        out += ' ';
        out += truncate_left(pool.get(job.display), job_path_width);
        out += pup::ansi::clear_line;
        ++result.line_count;
    }

    result.text = out.intern(pool);
    return result;
}

auto render_simple(ProgressState const& state, std::string_view variant) -> StringId
{
    auto out = Buf {};
    auto done = state.completed + state.failed;

    if (!variant.empty()) {
        out += '[';
        out += variant;
        out += "] ";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%zu/%zu]", done, state.total);
    out += buf;

    return out.intern(global_pool());
}

auto format_duration(std::chrono::milliseconds ms) -> StringId
{
    auto secs = static_cast<std::size_t>(ms.count() / 1000);
    auto mins = secs / 60;
    secs = secs % 60;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu:%02zu", mins, secs);
    return global_pool().intern(buf);
}

auto clear_lines(std::size_t count, std::FILE* out) -> void
{
    if (count == 0) {
        return;
    }

    std::fputs(pup::ansi::carriage_return.data(), out);
    std::fputs(pup::ansi::clear_line.data(), out);

    for (std::size_t i = 1; i < count; ++i) {
        std::fputs(pup::ansi::move_up.data(), out);
        std::fputs(pup::ansi::clear_line.data(), out);
    }
    std::fflush(out);
}

auto display_progress(ProgressOutput const& output, std::size_t& prev_lines, std::FILE* out) -> void
{
    clear_lines(prev_lines, out);
    std::fputs(global_pool().get(output.text).data(), out);
    std::fflush(out);
    prev_lines = output.line_count;
}

auto finalize_progress(std::size_t& prev_lines, std::FILE* out) -> void
{
    clear_lines(prev_lines, out);
    prev_lines = 0;
}

} // namespace pup::exec
