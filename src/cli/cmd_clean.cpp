// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/output.hpp"
#include "pup/core/types.hpp"
#include "pup/index/reader.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>

namespace pup::cli {

namespace {

template<typename... Args>
auto vprint(std::string_view variant_name, char const* fmt, Args&&... args) -> void
{
    printf("[%.*s] ", static_cast<int>(variant_name.size()), variant_name.data());
    if constexpr (sizeof...(args) == 0) {
        printf("%s", fmt);
    } else {
        printf(fmt, std::forward<Args>(args)...);
    }
}

template<typename... Args>
auto veprint(std::string_view variant_name, char const* fmt, Args&&... args) -> void
{
    fprintf(stderr, "[%.*s] ", static_cast<int>(variant_name.size()), variant_name.data());
    if constexpr (sizeof...(args) == 0) {
        fprintf(stderr, "%s", fmt);
    } else {
        fprintf(stderr, fmt, std::forward<Args>(args)...);
    }
}

auto remove_indexed_outputs(
    std::filesystem::path const& index_path,
    std::filesystem::path const& root,
    OutputMode mode,
    std::string_view variant_name
) -> RemoveResult
{
    auto result = RemoveResult {};

    auto index_result = pup::index::read_index(index_path);
    if (!index_result) {
        fprintf(stderr, "Warning: Could not load index: %s\n", index_result.error().message.c_str());
        return result;
    }

    auto const& index = *index_result;

    for (auto const& file : index.files()) {
        if (file.type != pup::NodeType::Generated) {
            continue;
        }

        auto rel_path = std::filesystem::path { file.path };
        auto abs_path = root / rel_path;
        for (auto parent = abs_path.parent_path();
             !parent.empty() && parent != parent.parent_path();
             parent = parent.parent_path()) {
            result.output_dirs.insert(parent);
        }

        if (!std::filesystem::exists(abs_path)) {
            continue;
        }

        if (mode.dry_run) {
            vprint(variant_name, "Would remove: %s\n", file.path.c_str());
            ++result.removed_count;
            continue;
        }

        auto ec = std::error_code {};
        if (std::filesystem::remove(abs_path, ec)) {
            ++result.removed_count;
            if (mode.verbose) {
                vprint(variant_name, "Removed: %s\n", file.path.c_str());
            }
        } else if (ec) {
            veprint(variant_name, "Error removing %s: %s\n", file.path.c_str(), ec.message().c_str());
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

    auto index_path = ctx->build_dir / ".pup" / "index";
    if (!std::filesystem::exists(index_path)) {
        vprint(variant_name, "Nothing to clean (no index found)\n");
        return EXIT_SUCCESS;
    }

    auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };
    // Paths are source-relative. Generated files exist at build_dir.
    // For in-tree builds, build_dir == root, so this works for both cases.
    auto result = remove_indexed_outputs(index_path, ctx->build_dir, mode, variant_name);

    auto dirs_removed = remove_empty_directories(
        result.output_dirs, ctx->build_dir, ctx->root, mode
    );

    if (opts.dry_run) {
        vprint(variant_name, "Would remove %zu files, %zu directories\n", result.removed_count, dirs_removed);
    } else {
        vprint(variant_name, "Removed %zu files, %zu directories\n", result.removed_count, dirs_removed);
    }

    return result.error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

auto distclean_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        veprint(variant_name, "Error: No build directory found (use -B to specify)\n");
        return EXIT_FAILURE;
    }

    auto index_path = ctx->build_dir / ".pup" / "index";
    auto error_count = std::size_t { 0 };
    auto output_dirs = std::set<std::filesystem::path> {};

    auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };

    if (std::filesystem::exists(index_path)) {
        // Paths are source-relative. Generated files exist at build_dir.
        auto result = remove_indexed_outputs(index_path, ctx->build_dir, mode, variant_name);
        error_count += result.error_count;
        output_dirs = std::move(result.output_dirs);
    }

    auto pup_dir = ctx->build_dir / ".pup";
    if (std::filesystem::exists(pup_dir)) {
        if (opts.dry_run) {
            vprint(variant_name, "Would remove: %s\n", pup_dir.string().c_str());
        } else {
            if (opts.verbose) {
                vprint(variant_name, "Removing: %s\n", pup_dir.string().c_str());
            }
            std::filesystem::remove_all(pup_dir);
        }
    }

    auto config_path = ctx->build_dir / "tup.config";
    if (std::filesystem::exists(config_path)) {
        if (opts.dry_run) {
            vprint(variant_name, "Would remove: %s\n", config_path.string().c_str());
        } else {
            if (opts.verbose) {
                vprint(variant_name, "Removing: %s\n", config_path.string().c_str());
            }
            std::filesystem::remove(config_path);
        }
    }

    output_dirs.insert(ctx->build_dir);
    remove_empty_directories(output_dirs, ctx->build_dir, ctx->root, mode);

    if (!opts.dry_run) {
        vprint(variant_name, "Project reset complete\n");
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
