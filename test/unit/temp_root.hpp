// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>

namespace pup::test {

namespace detail {

inline auto claim_root(std::filesystem::path const& base) -> std::filesystem::path
{
    auto rng = std::random_device {};
    auto dist = std::uniform_int_distribution<unsigned int> { 0, 0xFFFFFFFF };
    for (auto attempt = 0; attempt < 64; ++attempt) {
        auto candidate = base / ("pup_test_" + std::to_string(dist(rng)));
        auto ec = std::error_code {};
        if (std::filesystem::create_directories(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return {};
}

inline auto resolve_root() -> std::filesystem::path
{
    // Scratch must be outside any putup project tree: a project rejects files under it that no rule owns.
    auto ec = std::error_code {};
    if (auto ambient = std::filesystem::temp_directory_path(ec); !ec && !ambient.empty()) {
        if (auto root = claim_root(ambient); !root.empty()) {
            return root;
        }
    }

#ifndef _WIN32
    // e2e fixtures exec binaries from scratch, so a host relying on /dev/shm needs it mounted exec.
    for (auto const* candidate : { "/var/tmp", "/dev/shm" }) {
        if (auto root = claim_root(candidate); !root.empty()) {
            return root;
        }
    }
#endif

    std::fprintf(stderr, "temp_root: no writable scratch directory\n");
    std::abort();
}

} // namespace detail

/// Writable scratch directory for this process, resolved once.
inline auto temp_root() -> std::filesystem::path const&
{
    static auto const root = detail::resolve_root();
    return root;
}

/// A path under this process's scratch directory; the caller creates whatever it needs there.
inline auto temp_path(std::string_view stem) -> std::filesystem::path
{
    return temp_root() / std::string { stem };
}

/// A created directory, unique within this process and across concurrent shards.
inline auto temp_dir(std::string_view stem) -> std::filesystem::path
{
    static auto counter = std::atomic<unsigned> { 0 };
    auto path = temp_root() / (std::string { stem } + "_" + std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path);
    return path;
}

} // namespace pup::test
