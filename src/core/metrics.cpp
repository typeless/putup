// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/metrics.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

namespace pup {

auto Metrics::operator+=(Metrics const& other) -> Metrics&
{
    tupfiles_parsed += other.tupfiles_parsed;
    files_checked += other.files_checked;
    files_changed += other.files_changed;
    hash_computations += other.hash_computations;
    hashes_skipped += other.hashes_skipped;
    stat_calls += other.stat_calls;
    index_load_time += other.index_load_time;
    index_save_time += other.index_save_time;
    command_index_time += other.command_index_time;
    change_detection_time += other.change_detection_time;
    implicit_deps_time += other.implicit_deps_time;
    new_commands_time += other.new_commands_time;
    stale_outputs_time += other.stale_outputs_time;
    job_list_time += other.job_list_time;
    command_expansions += other.command_expansions;
    return *this;
}

namespace {
auto g_metrics = Metrics {};
} // namespace

auto thread_metrics() -> Metrics&
{
    return g_metrics;
}

auto collect_metrics() -> Metrics
{
    auto result = g_metrics;
    g_metrics = Metrics {};

    auto& pool = global_pool();
    result.pool_strings = pool.size();
    result.pool_bytes = pool.bytes();

    return result;
}

auto reset_metrics() -> void
{
    g_metrics = Metrics {};
}

} // namespace pup
