// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/metrics.hpp"

#include <mutex>
#include <vector>

namespace pup {

auto Metrics::operator+=(Metrics const& other) -> Metrics&
{
    tupfiles_parsed += other.tupfiles_parsed;
    files_checked += other.files_checked;
    files_changed += other.files_changed;
    hash_computations += other.hash_computations;
    stat_calls += other.stat_calls;
    index_load_time += other.index_load_time;
    index_save_time += other.index_save_time;
    return *this;
}

namespace {

auto registry_mutex() -> std::mutex&
{
    static auto mtx = std::mutex {};
    return mtx;
}

auto metrics_registry() -> std::vector<Metrics*>&
{
    static auto registry = std::vector<Metrics*> {};
    return registry;
}

auto register_metrics(Metrics* m) -> void
{
    auto lock = std::lock_guard { registry_mutex() };
    metrics_registry().push_back(m);
}

} // namespace

auto thread_metrics() -> Metrics&
{
    thread_local auto metrics = Metrics {};
    thread_local auto registered = false;

    if (!registered) {
        register_metrics(&metrics);
        registered = true;
    }

    return metrics;
}

auto collect_metrics() -> Metrics
{
    auto lock = std::lock_guard { registry_mutex() };
    auto result = Metrics {};

    for (auto* m : metrics_registry()) {
        result += *m;
        *m = Metrics {};
    }

    return result;
}

auto reset_metrics() -> void
{
    auto lock = std::lock_guard { registry_mutex() };

    for (auto* m : metrics_registry()) {
        *m = Metrics {};
    }
}

} // namespace pup
