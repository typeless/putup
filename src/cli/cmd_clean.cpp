// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/output.hpp"
#include "pup/core/types.hpp"
#include "pup/index/reader.hpp"

#include <cstdlib>
#include <set>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto remove_indexed_outputs(
    std::filesystem::path const& index_path,
    std::filesystem::path const& root,
    OutputMode mode,
    std::string_view variant_name
) -> RemoveResult
{
    auto result = RemoveResult {};

    auto reader_result = pup::index::IndexReader::open(index_path);
    if (!reader_result) {
        return result;
    }

    auto index_result = reader_result->read();
    if (!index_result) {
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
            fmt::print("[{}] Would remove: {}\n", variant_name, file.path);
            ++result.removed_count;
            continue;
        }

        auto ec = std::error_code {};
        if (std::filesystem::remove(abs_path, ec)) {
            ++result.removed_count;
            if (mode.verbose) {
                fmt::print("[{}] Removed: {}\n", variant_name, file.path);
            }
        } else if (ec) {
            fmt::print(stderr, "[{}] Error removing {}: {}\n", variant_name, file.path, ec.message());
            ++result.error_count;
        }
    }

    return result;
}

auto clean_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        fmt::print(stderr, "[{}] Error: No build directory found (use -B to specify)\n", variant_name);
        return EXIT_FAILURE;
    }

    auto index_path = ctx->build_dir / ".pup" / "index";
    if (!std::filesystem::exists(index_path)) {
        fmt::print("[{}] Nothing to clean (no index found)\n", variant_name);
        return EXIT_SUCCESS;
    }

    auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };
    // Use build_dir for outputs (source-root-relative paths stored in index)
    auto result = remove_indexed_outputs(index_path, ctx->build_dir, mode, variant_name);

    auto dirs_removed = remove_empty_directories(
        result.output_dirs, ctx->build_dir, ctx->root, mode
    );

    if (opts.dry_run) {
        fmt::print("[{}] Would remove {} files, {} directories\n", variant_name, result.removed_count, dirs_removed);
    } else {
        fmt::print("[{}] Removed {} files, {} directories\n", variant_name, result.removed_count, dirs_removed);
    }

    return result.error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

auto distclean_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        fmt::print(stderr, "[{}] Error: No build directory found (use -B to specify)\n", variant_name);
        return EXIT_FAILURE;
    }

    auto index_path = ctx->build_dir / ".pup" / "index";
    auto error_count = std::size_t { 0 };
    auto output_dirs = std::set<std::filesystem::path> {};

    auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };

    if (std::filesystem::exists(index_path)) {
        // Use build_dir for outputs (source-root-relative paths stored in index)
        auto result = remove_indexed_outputs(index_path, ctx->build_dir, mode, variant_name);
        error_count += result.error_count;
        output_dirs = std::move(result.output_dirs);
    }

    auto pup_dir = ctx->build_dir / ".pup";
    if (std::filesystem::exists(pup_dir)) {
        if (opts.dry_run) {
            fmt::print("[{}] Would remove: {}\n", variant_name, pup_dir.string());
        } else {
            if (opts.verbose) {
                fmt::print("[{}] Removing: {}\n", variant_name, pup_dir.string());
            }
            std::filesystem::remove_all(pup_dir);
        }
    }

    auto config_path = ctx->build_dir / "tup.config";
    if (std::filesystem::exists(config_path)) {
        if (opts.dry_run) {
            fmt::print("[{}] Would remove: {}\n", variant_name, config_path.string());
        } else {
            if (opts.verbose) {
                fmt::print("[{}] Removing: {}\n", variant_name, config_path.string());
            }
            std::filesystem::remove(config_path);
        }
    }

    output_dirs.insert(ctx->build_dir);
    remove_empty_directories(output_dirs, ctx->build_dir, ctx->root, mode);

    if (!opts.dry_run) {
        fmt::print("[{}] Project reset complete\n", variant_name);
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
