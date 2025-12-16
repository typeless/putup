// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include <chrono>
#include <cstddef>

namespace pup {

struct Metrics {
    std::size_t tupfiles_parsed = 0;
    std::size_t files_checked = 0;
    std::size_t files_changed = 0;
    std::size_t hash_computations = 0;
    std::size_t stat_calls = 0;
    std::chrono::milliseconds index_load_time { 0 };
    std::chrono::milliseconds index_save_time { 0 };

    auto operator+=(Metrics const& other) -> Metrics&;
};

/// Thread-local metrics for current thread
auto thread_metrics() -> Metrics&;

/// Collect, merge, and reset all thread-local metrics (consumes values)
auto collect_metrics() -> Metrics;

/// Reset all metrics (for testing)
auto reset_metrics() -> void;

} // namespace pup
