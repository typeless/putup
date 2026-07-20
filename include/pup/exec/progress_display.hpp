// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/clock.hpp"
#include "pup/core/print.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <chrono>
#include <string_view>

namespace pup::exec {

/// Single running job with timing
struct RunningJob {
    NodeId id = 0;
    StringId display = StringId::Empty;
    pup::SteadyClock::time_point start_time = {};
};

/// Progress state - pure data, no methods
struct ProgressState {
    std::size_t total = 0;
    std::size_t completed = 0;
    std::size_t failed = 0;
    Vec<RunningJob> running = {};
};

/// Rendered output ready for display
struct ProgressOutput {
    StringId text = StringId::Empty;
    std::size_t line_count = 0;
};

// ============================================================================
// Pure transform functions (state -> state)
// ============================================================================

/// Add a job to the running set
[[nodiscard]]
auto job_started(ProgressState state, NodeId id, StringId display) -> ProgressState;

/// Remove a job and update counts
[[nodiscard]]
auto job_completed(ProgressState state, NodeId id, bool success) -> ProgressState;

// ============================================================================
// Pure render functions (state -> output)
// ============================================================================

/// Render progress for TTY (multi-line with job list)
[[nodiscard]]
auto render_tty(ProgressState const& state, std::string_view variant = {}) -> ProgressOutput;

/// Render progress for non-TTY (single line)
[[nodiscard]]
auto render_simple(ProgressState const& state, std::string_view variant = {}) -> StringId;

/// Format duration as M:SS
[[nodiscard]]
auto format_duration(std::chrono::milliseconds ms) -> StringId;

// ============================================================================
// Terminal I/O (side effects isolated here)
// ============================================================================

/// Clear N lines above cursor and return to line start
auto clear_lines(std::size_t count, Stream stream = Stream::Out) -> void;

/// Print progress output, clearing previous if needed
auto display_progress(ProgressOutput const& output, std::size_t& prev_lines, Stream stream = Stream::Out) -> void;

/// Clear the progress display and print final newline
auto finalize_progress(std::size_t& prev_lines, Stream stream = Stream::Out) -> void;

} // namespace pup::exec
