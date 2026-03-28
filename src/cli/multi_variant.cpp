// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/multi_variant.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/target.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include "pup/platform/process.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace pup::cli {

namespace {

struct ParsedTargets {
    Vec<StringId> variants;
    Vec<StringId> scopes;
    Vec<StringId> output_targets;
    bool has_variant_targets = false;
};

auto parse_targets_for_variants(
    std::string_view source_root,
    Vec<StringId> const& targets
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

    auto& pool = global_pool();
    for (auto const& target : *parsed) {
        if (target.variant.has_value()) {
            result.has_variant_targets = true;
            result.variants.push_back(*target.variant);
            if (!is_empty(target.scope_or_output)) {
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

    // Deduplicate variant names
    std::sort(result.variants.begin(), result.variants.end(), [&pool](auto a, auto b) {
        return pool.get(a) < pool.get(b);
    });
    result.variants.erase(std::unique(result.variants.begin(), result.variants.end()), result.variants.end());

    return result;
}

} // namespace

auto for_each_variant(
    Options const& opts,
    VariantHandler handler,
    std::string_view command_name
) -> int
{
    auto& pool = global_pool();
    auto layout_result = Result<ProjectLayout> { discover_layout(make_layout_options(opts)) };
    if (!layout_result) {
        fprintf(stderr, "Error: %s\n", layout_result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto source_root = layout_result->source_root;
    auto source_root_sv = pool.get(source_root);

    auto parsed_targets = parse_targets_for_variants(source_root_sv, opts.targets);
    if (!parsed_targets.has_value()) {
        fprintf(stderr, "Error: %s\n", parsed_targets.error().msg().data());
        return EXIT_FAILURE;
    }

    auto variants = Vec<StringId> {};
    auto scopes = Vec<StringId> {};
    auto output_targets = Vec<StringId> {};

    if (parsed_targets->has_variant_targets) {
        variants = parsed_targets->variants;
        scopes = parsed_targets->scopes;
        output_targets = parsed_targets->output_targets;
    } else if (!opts.build_dirs.empty()) {
        for (auto dir : opts.build_dirs) {
            variants.push_back(dir);
        }
        scopes = parsed_targets->scopes;
        output_targets = parsed_targets->output_targets;
    } else {
        variants = discover_variants(source_root_sv);
        scopes = parsed_targets->scopes;
        output_targets = parsed_targets->output_targets;
    }

    if (variants.empty()) {
        auto modified_opts = Options { opts };
        modified_opts.targets = scopes;
        modified_opts.output_targets = output_targets;
        return handler(modified_opts, ".");
    }

    auto cwd_id = *pup::platform::current_directory();
    auto cwd = pool.get(cwd_id);
    auto cwd_variant = std::optional<StringId> {};
    for (auto variant : variants) {
        auto variant_abs = pool.get(pup::path::join(source_root_sv, pool.get(variant)));
        if (pup::is_path_under(cwd, variant_abs)) {
            cwd_variant = variant;
            break;
        }
    }

    if (cwd_variant) {
        auto single_opts = Options { opts };
        single_opts.targets = scopes;
        single_opts.output_targets = output_targets;
        return handler(single_opts, pup::path::filename(pool.get(*cwd_variant)));
    }

    if (variants.size() == 1) {
        auto single_opts = Options { opts };
        single_opts.build_dirs = Vec<StringId> { variants[0] };
        single_opts.targets = scopes;
        single_opts.output_targets = output_targets;
        return handler(single_opts, pup::path::filename(pool.get(variants[0])));
    }

    if (opts.verbose) {
        printf("%.*s %zu variants in parallel:\n", static_cast<int>(command_name.size()), command_name.data(), variants.size());
        for (auto v : variants) {
            printf("  %s\n", pool.get(v).data());
        }
    }

    struct TaskContext {
        Options const* opts;
        VariantHandler const* handler;
        StringPool* pool;
        Vec<StringId> const* scopes;
        Vec<StringId> const* output_targets;
        StringId variant;
    };

    auto task_fn = [](void* ctx_ptr) -> int {
        auto* ctx = static_cast<TaskContext*>(ctx_ptr);
        auto variant_opts = Options { *ctx->opts };
        variant_opts.build_dirs = Vec<StringId> { ctx->variant };
        variant_opts.targets = *ctx->scopes;
        variant_opts.output_targets = *ctx->output_targets;
        return (*ctx->handler)(variant_opts, pup::path::filename(ctx->pool->get(ctx->variant)));
    };

    auto task_contexts = Vec<TaskContext> {};
    auto context_ptrs = Vec<void*> {};
    task_contexts.reserve(variants.size());
    context_ptrs.reserve(variants.size());

    for (auto variant : variants) {
        task_contexts.push_back(TaskContext {
            .opts = &opts,
            .handler = &handler,
            .pool = &pool,
            .scopes = &scopes,
            .output_targets = &output_targets,
            .variant = variant,
        });
    }
    for (std::size_t i = 0; i < task_contexts.size(); ++i) {
        context_ptrs.push_back(&task_contexts[i]);
    }

    auto failed = pup::platform::run_parallel_tasks(task_fn, context_ptrs.data(), context_ptrs.size());

    if (failed > 0) {
        fprintf(stderr, "%d of %zu variants failed\n", failed, variants.size());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace pup::cli
