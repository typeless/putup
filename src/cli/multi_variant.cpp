// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/multi_variant.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/target.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/result.hpp"
#include "pup/platform/file_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <future>
#include <set>
#include <vector>

namespace pup::cli {

namespace {

struct ParsedTargets {
    std::vector<std::string> variants;
    std::vector<std::string> scopes;
    std::vector<std::string> output_targets;
    bool has_variant_targets = false;
};

auto parse_targets_for_variants(
    std::string const& source_root,
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
            variant_set.insert(*target.variant);
            if (!target.scope_or_output.empty()) {
                if (target.is_output) {
                    result.output_targets.push_back(target.scope_or_output);
                } else {
                    result.scopes.push_back(target.scope_or_output);
                }
            }
        } else {
            if (target.is_output) {
                result.output_targets.push_back(target.scope_or_output);
            } else {
                result.scopes.push_back(target.scope_or_output);
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
    auto layout_result = Result<ProjectLayout> { discover_layout(make_layout_options(opts)) };
    if (!layout_result) {
        fprintf(stderr, "Error: %s\n", layout_result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto const& source_root = layout_result->source_root;

    auto parsed_targets = parse_targets_for_variants(source_root, opts.targets);
    if (!parsed_targets.has_value()) {
        fprintf(stderr, "Error: %s\n", parsed_targets.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto variants = std::vector<std::string> {};
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

    if (variants.empty()) {
        auto modified_opts = Options { opts };
        modified_opts.targets = scopes;
        modified_opts.output_targets = output_targets;
        return handler(modified_opts, ".");
    }

    auto cwd = *pup::platform::current_directory();
    auto cwd_variant = std::optional<std::string> {};
    for (auto const& variant : variants) {
        auto variant_abs = pup::path::join(source_root, variant);
        if (pup::is_path_under(cwd, variant_abs)) {
            cwd_variant = variant;
            break;
        }
    }

    if (cwd_variant) {
        auto single_opts = Options { opts };
        single_opts.targets = scopes;
        single_opts.output_targets = output_targets;
        return handler(single_opts, std::string { pup::path::filename(*cwd_variant) });
    }

    if (variants.size() == 1) {
        auto single_opts = Options { opts };
        single_opts.build_dirs = { variants[0] };
        single_opts.targets = scopes;
        single_opts.output_targets = output_targets;
        return handler(single_opts, std::string { pup::path::filename(variants[0]) });
    }

    if (opts.verbose) {
        printf("%.*s %zu variants in parallel:\n", static_cast<int>(command_name.size()), command_name.data(), variants.size());
        for (auto const& v : variants) {
            printf("  %s\n", v.c_str());
        }
    }

    auto futures = std::vector<std::future<int>> {};
    for (auto const& variant : variants) {
        futures.push_back(std::async(
            std::launch::async,
            [&opts, &handler, &scopes, &output_targets, variant = variant] {
                auto variant_opts = Options { opts };
                variant_opts.build_dirs = { variant };
                variant_opts.targets = scopes;
                variant_opts.output_targets = output_targets;
                return handler(variant_opts, std::string { pup::path::filename(variant) });
            }
        ));
    }

    auto failed = 0;
    for (auto& future : futures) {
        if (future.get() != 0) {
            ++failed;
        }
    }

    if (failed > 0) {
        fprintf(stderr, "%d of %zu variants failed\n", failed, variants.size());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace pup::cli
