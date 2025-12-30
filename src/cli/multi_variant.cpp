// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/multi_variant.hpp"
#include "pup/cli/target.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/result.hpp"

#include <cstdlib>
#include <future>
#include <set>
#include <vector>

#include <fmt/core.h>

namespace pup::cli {

namespace {

struct ParsedTargets {
    std::vector<std::filesystem::path> variants;
    std::vector<std::string> scopes;
    std::vector<std::string> output_targets; // Specific output file targets
    bool has_variant_targets = false;
};

auto parse_targets_for_variants(
    std::filesystem::path const& source_root,
    std::vector<std::string> const& targets
) -> pup::Result<ParsedTargets>
{
    auto result = ParsedTargets {};

    if (targets.empty()) {
        return result;
    }

    auto parsed = validate_target_consistency(source_root, targets);
    if (!parsed.has_value()) {
        return pup::unexpected<pup::Error> { parsed.error() };
    }

    auto variant_set = std::set<std::string> {};
    for (auto const& target : *parsed) {
        if (target.variant.has_value()) {
            result.has_variant_targets = true;
            variant_set.insert(target.variant->string());
            if (!target.scope_or_output.empty()) {
                if (target.is_output) {
                    // Store source-root-relative path (graph uses source-root-relative)
                    result.output_targets.push_back(target.scope_or_output.string());
                } else {
                    result.scopes.push_back(target.scope_or_output.string());
                }
            }
        } else {
            if (target.is_output) {
                result.output_targets.push_back(target.scope_or_output.string());
            } else {
                result.scopes.push_back(target.scope_or_output.string());
            }
        }
    }

    for (auto const& v : variant_set) {
        result.variants.emplace_back(v);
    }

    return result;
}

} // namespace

auto for_each_variant(
    Options const& opts,
    VariantHandler handler,
    std::string_view command_name
) -> int
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

    // Parse targets to extract variants and scopes
    auto parsed_targets = parse_targets_for_variants(source_root, opts.targets);
    if (!parsed_targets.has_value()) {
        fmt::print(stderr, "Error: {}\n", parsed_targets.error().message);
        return EXIT_FAILURE;
    }

    // Determine variants to process
    auto variants = std::vector<std::filesystem::path> {};
    auto scopes = std::vector<std::string> {};
    auto output_targets = std::vector<std::string> {};

    if (parsed_targets->has_variant_targets) {
        variants = parsed_targets->variants;
        scopes = parsed_targets->scopes;
        output_targets = parsed_targets->output_targets;
    } else if (!opts.build_dirs.empty()) {
        for (auto const& dir : opts.build_dirs) {
            variants.emplace_back(dir);
        }
        scopes = parsed_targets->scopes;
        output_targets = parsed_targets->output_targets;
    } else {
        variants = discover_variants(source_root);
        scopes = parsed_targets->scopes;
        output_targets = parsed_targets->output_targets;
    }

    // No variants found - in-tree operation
    if (variants.empty()) {
        auto modified_opts = Options { opts };
        modified_opts.targets = scopes;
        modified_opts.output_targets = output_targets;
        return handler(modified_opts, ".");
    }

    // Single variant - direct call
    if (variants.size() == 1) {
        auto single_opts = Options { opts };
        single_opts.build_dirs = { variants[0].string() };
        single_opts.targets = scopes;
        single_opts.output_targets = output_targets;
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
        futures.push_back(std::async(
            std::launch::async,
            [&opts, &handler, &scopes, &output_targets, variant = variant] {
                auto variant_opts = Options { opts };
                variant_opts.build_dirs = { variant.string() };
                variant_opts.targets = scopes;
                variant_opts.output_targets = output_targets;
                return handler(variant_opts, variant.filename().string());
            }
        ));
    }

    // Collect results
    auto failed = 0;
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
