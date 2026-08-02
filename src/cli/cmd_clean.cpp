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
#include "pup/core/types.hpp"
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
    Vec<StringId>& output_dirs
) -> RemoveResult
{
    auto result = RemoveResult {};

    auto index_result = pup::index::read_index(index_path);
    if (!index_result) {
        eprint("Warning: Could not load index: {}\n", index_result.error().msg());
        return result;
    }

    auto const& index = *index_result;

    for (auto const& file : index.files()) {
        if (file.type != pup::NodeType::Generated) {
            continue;
        }

        auto file_path_sv = pup::global_pool().get(file.path);
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
            continue;
        }

        auto r = pup::platform::remove_file(abs_path_sv);
        if (r) {
            ++result.removed_count;
            if (mode.verbose) {
                vprint(variant_name, "Removed: {}\n", file_path_sv);
            }
        } else {
            veprint(variant_name, "Error removing {}: {}\n", file_path_sv, r.error().msg());
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
    auto result = remove_indexed_outputs(index_path_sv, root_sv, mode, variant_name, output_dirs);

    auto dirs = remove_empty_directories(output_dirs, build_dir_sv, root_sv, mode);

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

    auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };

    if (pup::platform::exists(index_path_sv)) {
        auto result = remove_indexed_outputs(index_path_sv, root_sv, mode, variant_name, output_dirs);
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
                veprint(variant_name, "Error removing {}: {}\n", pup_dir_sv, r.error().msg());
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
                veprint(variant_name, "Error removing {}: {}\n", config_path_sv, r.error().msg());
                ++error_count;
            }
        }
    }

    output_dirs.push_back(pool.intern(build_dir_sv));
    error_count += remove_empty_directories(output_dirs, build_dir_sv, root_sv, mode).error_count;

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
