// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <chrono>
#include <cstddef>

namespace pup {

struct Metrics {
    std::size_t tupfiles_parsed = 0;
    std::size_t files_checked = 0;
    std::size_t files_changed = 0;
    std::size_t hash_computations = 0;
    std::size_t hashes_skipped = 0; ///< Hash computations skipped due to stat cache hit
    std::size_t stat_calls = 0;
    std::chrono::milliseconds index_load_time { 0 };
    std::chrono::milliseconds index_save_time { 0 };

    // Phase timing (microseconds for precision)
    std::chrono::microseconds command_index_time { 0 };
    std::chrono::microseconds change_detection_time { 0 };
    std::chrono::microseconds implicit_deps_time { 0 };
    std::chrono::microseconds new_commands_time { 0 };
    std::chrono::microseconds stale_outputs_time { 0 };
    std::chrono::microseconds job_list_time { 0 };

    // Command expansion stats
    std::size_t command_expansions = 0;

    auto operator+=(Metrics const& other) -> Metrics&;
};

/// Thread-local metrics for current thread
auto thread_metrics() -> Metrics&;

/// Collect, merge, and reset all thread-local metrics (consumes values)
auto collect_metrics() -> Metrics;

/// Reset all metrics (for testing)
auto reset_metrics() -> void;

} // namespace pup
