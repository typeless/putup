// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/options.hpp"
#include "pup/cli/output.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/print.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/index/reader.hpp"
#include "pup/platform/file_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace pup::cli {

namespace {

auto pool_get(StringId id) -> std::string_view
{
    return global_pool().get(id);
}

template<typename... Args>
auto vprint(std::string_view variant_name, char const* fmt, Args&&... args) -> void
{
    print("[{}] ", variant_name);
    print(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
auto veprint(std::string_view variant_name, char const* fmt, Args&&... args) -> void
{
    eprint("[{}] ", variant_name);
    eprint(fmt, std::forward<Args>(args)...);
}

auto remove_indexed_outputs(
    std::string_view index_path,
    std::string_view root,
    OutputMode mode,
    std::string_view variant_name,
    Vec<StringId>& output_dirs,
    Vec<StringId>& removed_paths
) -> RemoveResult
{
    auto result = RemoveResult {};

    // Which outputs the record owns, not whether they are current: a record too old for
    // read_index still knows what it produced, and that is the whole question here (#291).
    auto const prior = pup::index::read_prior_paths(index_path);
    if (prior.kind != pup::index::PriorPaths::Kind::Known) {
        eprint("Warning: Could not read the build record, so no generated file can be named\n");
        return result;
    }

    for (auto path_id : prior.generated) {
        auto file_path_sv = pup::global_pool().get(path_id);
        auto abs_path_sv = pup::global_pool().get(pup::path::join(root, file_path_sv));
        for (auto parent = pup::path::parent(abs_path_sv);
             !parent.empty() && parent != pup::path::parent(parent);
             parent = pup::path::parent(parent)) {
            output_dirs.push_back(pup::global_pool().intern(parent));
        }

        if (!pup::platform::exists(abs_path_sv)) {
            continue;
        }

        if (mode.dry_run) {
            vprint(variant_name, "Would remove: {}\n", file_path_sv);
            ++result.removed_count;
            removed_paths.push_back(pup::global_pool().intern(abs_path_sv));
            continue;
        }

        auto r = pup::platform::remove_file(abs_path_sv);
        if (r) {
            ++result.removed_count;
            removed_paths.push_back(pup::global_pool().intern(abs_path_sv));
            if (mode.verbose) {
                vprint(variant_name, "Removed: {}\n", file_path_sv);
            }
        } else {
            veprint(variant_name, "{}\n", r.error().msg());
            ++result.error_count;
        }
    }

    return result;
}

auto clean_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        veprint(variant_name, "Error: No build directory found (use -B to specify)\n");
        return EXIT_FAILURE;
    }

    auto build_dir_sv = pool_get(ctx->build_dir);
    auto root_sv = pool_get(ctx->root);
    auto index_path_sv = pup::global_pool().get(pup::path::join(pup::global_pool().get(pup::path::join(build_dir_sv, ".pup")), "index"));
    if (!pup::platform::exists(index_path_sv)) {
        vprint(variant_name, "Nothing to clean (no index found)\n");
        return EXIT_SUCCESS;
    }

    auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };
    auto output_dirs = Vec<StringId> {};
    auto removed_paths = Vec<StringId> {};
    auto result = remove_indexed_outputs(index_path_sv, root_sv, mode, variant_name, output_dirs, removed_paths);

    auto dirs = remove_empty_directories(output_dirs, build_dir_sv, root_sv, mode, variant_name, removed_paths);

    if (opts.dry_run) {
        vprint(variant_name, "Would remove {} files, {} directories\n", result.removed_count, dirs.removed_count);
    } else {
        vprint(variant_name, "Removed {} files, {} directories\n", result.removed_count, dirs.removed_count);
    }

    return result.error_count + dirs.error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

auto distclean_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        veprint(variant_name, "Error: No build directory found (use -B to specify)\n");
        return EXIT_FAILURE;
    }

    auto& pool = pup::global_pool();
    auto build_dir_sv = pool.get(ctx->build_dir);
    auto root_sv = pool.get(ctx->root);
    auto index_path_sv = pool.get(pup::path::join(pool.get(pup::path::join(build_dir_sv, ".pup")), "index"));
    auto error_count = std::size_t { 0 };
    auto output_dirs = Vec<StringId> {};
    auto removed_paths = Vec<StringId> {};

    auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };

    if (pup::platform::exists(index_path_sv)) {
        auto result = remove_indexed_outputs(index_path_sv, root_sv, mode, variant_name, output_dirs, removed_paths);
        error_count += result.error_count;
    }

    auto pup_dir_sv = pool.get(pup::path::join(build_dir_sv, ".pup"));
    if (pup::platform::exists(pup_dir_sv)) {
        if (opts.dry_run) {
            vprint(variant_name, "Would remove: {}\n", pup_dir_sv);
        } else {
            if (opts.verbose) {
                vprint(variant_name, "Removing: {}\n", pup_dir_sv);
            }
            if (auto r = pup::platform::remove_all(pup_dir_sv); !r) {
                veprint(variant_name, "{}\n", r.error().msg());
                ++error_count;
            }
        }
    }

    auto config_path_sv = pool.get(pup::path::join(build_dir_sv, "tup.config"));
    if (pup::platform::exists(config_path_sv)) {
        if (opts.dry_run) {
            vprint(variant_name, "Would remove: {}\n", config_path_sv);
        } else {
            if (opts.verbose) {
                vprint(variant_name, "Removing: {}\n", config_path_sv);
            }
            if (auto r = pup::platform::remove_file(config_path_sv); !r) {
                veprint(variant_name, "{}\n", r.error().msg());
                ++error_count;
            }
        }
    }

    output_dirs.push_back(pool.intern(build_dir_sv));
    error_count += remove_empty_directories(output_dirs, build_dir_sv, root_sv, mode, variant_name, removed_paths).error_count;

    if (!opts.dry_run) {
        if (error_count > 0) {
            veprint(variant_name, "Project reset incomplete: {} errors\n", error_count);
        } else {
            vprint(variant_name, "Project reset complete\n");
        }
    }

    return error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace

auto cmd_clean(Options const& opts) -> int
{
    return for_each_variant(opts, clean_single_variant, "Cleaning");
}

auto cmd_distclean(Options const& opts) -> int
{
    return for_each_variant(opts, distclean_single_variant, "Distcleaning");
}

} // namespace pup::cli
