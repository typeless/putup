// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/multi_variant.hpp"
#include "pup/core/layout.hpp"

#include <cstdlib>
#include <future>
#include <vector>

#include <fmt/core.h>

namespace pup::cli {

auto for_each_variant(
    Options const& opts,
    VariantHandler handler,
    std::string_view command_name) -> int
{
    // Discover source root
    auto layout_opts = LayoutOptions {};
    if (!opts.source_dir.empty()) {
        layout_opts.source_dir = std::filesystem::path { opts.source_dir };
    }
    if (!opts.build_dirs.empty()) {
        layout_opts.build_dir = std::filesystem::path { opts.build_dirs[0] };
    }

    auto layout_result = Result<ProjectLayout> { discover_layout(layout_opts) };
    if (!layout_result) {
        fmt::print(stderr, "Error: {}\n", layout_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& source_root = layout_result->source_root;

    // Determine variants to process
    auto variants = std::vector<std::filesystem::path> {};
    if (!opts.build_dirs.empty()) {
        for (auto const& dir : opts.build_dirs) {
            variants.emplace_back(dir);
        }
    } else {
        variants = discover_variants(source_root);
    }

    // No variants found - in-tree operation
    if (variants.empty()) {
        return handler(opts, ".");
    }

    // Single variant - direct call
    if (variants.size() == 1) {
        auto single_opts = Options { opts };
        single_opts.build_dirs = { variants[0].string() };
        return handler(single_opts, variants[0].filename().string());
    }

    // Multiple variants - parallel execution
    if (opts.verbose) {
        fmt::print("{} {} variants in parallel:\n", command_name, variants.size());
        for (auto const& v : variants) {
            fmt::print("  {}\n", v.string());
        }
    }

    auto futures = std::vector<std::future<int>> {};
    for (auto const& variant : variants) {
        // Capture variant by value to avoid dangling reference
        futures.push_back(std::async(std::launch::async, [&opts, &handler, variant = variant] {
            auto variant_opts = Options { opts };
            variant_opts.build_dirs = { variant.string() };
            return handler(variant_opts, variant.filename().string());
        }));
    }

    // Collect results
    auto failed = int { 0 };
    for (auto& future : futures) {
        if (future.get() != 0) {
            ++failed;
        }
    }

    if (failed > 0) {
        fmt::print(stderr, "{} of {} variants failed\n", failed, variants.size());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace pup::cli
