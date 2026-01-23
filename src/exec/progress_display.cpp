// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/exec/progress_display.hpp"
#include "pup/core/terminal.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace pup::exec {

namespace {

auto constexpr MAX_RUNNING_JOBS_DISPLAY = std::size_t { 8 };

/// Truncate string from the left if longer than max_width, adding "..." prefix
auto truncate_left(std::string_view str, std::size_t max_width) -> std::string
{
    if (str.size() <= max_width) {
        return std::string { str };
    }
    if (max_width <= 3) {
        return std::string(max_width, '.');
    }
    return "..." + std::string { str.substr(str.size() - (max_width - 3)) };
}

} // anonymous namespace

auto job_started(ProgressState state, NodeId id, std::string display) -> ProgressState
{
    state.running.push_back(RunningJob {
        .id = id,
        .display = std::move(display),
        .start_time = std::chrono::steady_clock::now(),
    });
    return state;
}

auto job_completed(ProgressState state, NodeId id, bool success) -> ProgressState
{
    std::erase_if(state.running, [id](auto const& j) { return j.id == id; });
    if (success) {
        ++state.completed;
    } else {
        ++state.failed;
    }
    return state;
}

auto render_tty(ProgressState const& state, std::string_view variant) -> ProgressOutput
{
    auto result = ProgressOutput {};
    auto out = std::ostringstream {};

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
        current_display = sorted.back().display;
    }

    // Build progress line prefix to calculate available width
    auto prefix = std::ostringstream {};
    if (!variant.empty()) {
        prefix << "[" << variant << "] ";
    }
    prefix << "[";
    if (pct < 10) {
        prefix << "  ";
    } else if (pct < 100) {
        prefix << " ";
    }
    prefix << pct << "% " << done << "/" << state.total << "] ";
    auto prefix_str = prefix.str();
    auto path_width = term_width > prefix_str.size() ? term_width - prefix_str.size() : std::size_t { 20 };

    out << prefix_str << truncate_left(current_display, path_width) << pup::ansi::clear_line;
    result.line_count = 1;

    // Exclude the last job (shown on progress line) from the running list
    auto list_size = sorted.size() > 1 ? sorted.size() - 1 : std::size_t { 0 };
    auto max_jobs = std::min(list_size, MAX_RUNNING_JOBS_DISPLAY);

    // Running jobs prefix: "    M:SS " = 9 chars minimum (more for longer times)
    auto constexpr job_prefix_width = std::size_t { 9 };
    auto job_path_width = term_width > job_prefix_width ? term_width - job_prefix_width : std::size_t { 20 };

    for (std::size_t i = 0; i < max_jobs; ++i) {
        auto const& job = sorted[i];
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.start_time);
        out << "\n    " << format_duration(elapsed) << " " << truncate_left(job.display, job_path_width) << pup::ansi::clear_line;
        ++result.line_count;
    }

    result.text = out.str();
    return result;
}

auto render_simple(ProgressState const& state, std::string_view variant) -> std::string
{
    auto out = std::ostringstream {};
    auto done = state.completed + state.failed;

    if (!variant.empty()) {
        out << "[" << variant << "] ";
    }
    out << "[" << done << "/" << state.total << "]";

    return out.str();
}

auto format_duration(std::chrono::milliseconds ms) -> std::string
{
    auto secs = static_cast<std::size_t>(ms.count() / 1000);
    auto mins = secs / 60;
    secs = secs % 60;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu:%02zu", mins, secs);
    return buf;
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
    std::fputs(output.text.c_str(), out);
    std::fflush(out);
    prev_lines = output.line_count;
}

auto finalize_progress(std::size_t& prev_lines, std::FILE* out) -> void
{
    clear_lines(prev_lines, out);
    prev_lines = 0;
}

} // namespace pup::exec
