// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
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
    OutputMode mode) -> RemoveResult
{
    auto result = RemoveResult {};

    auto reader_result = pup::index::IndexReader::open(index_path);
    if (!reader_result)
        return result;

    auto index_result = reader_result->read();
    if (!index_result)
        return result;

    auto const& index = *index_result;

    for (auto const& file : index.files()) {
        if (file.type != pup::NodeType::Generated)
            continue;

        auto path = std::filesystem::path { file.path };
        for (auto parent = path.parent_path();
             !parent.empty() && parent != parent.parent_path();
             parent = parent.parent_path())
            result.output_dirs.insert(parent);

        if (!std::filesystem::exists(path))
            continue;

        if (mode.dry_run) {
            fmt::print("Would remove: {}\n", file.path);
            ++result.removed_count;
            continue;
        }

        auto ec = std::error_code {};
        if (std::filesystem::remove(path, ec)) {
            ++result.removed_count;
            if (mode.verbose)
                fmt::print("Removed: {}\n", file.path);
        } else if (ec) {
            fmt::print(stderr, "Error removing {}: {}\n", file.path, ec.message());
            ++result.error_count;
        }
    }

    return result;
}

auto with_clean_context(Options const& opts, auto&& handler) -> int
{
    auto ctx = resolve_clean_context(opts);
    if (!ctx) {
        fmt::print(stderr, "Error: No build directory found (use -B to specify)\n");
        return EXIT_FAILURE;
    }
    return handler(*ctx);
}

} // namespace

auto cmd_clean(Options const& opts) -> int
{
    return with_clean_context(opts, [&](CleanContext& ctx) {
        auto index_path = ctx.build_dir / ".pup" / "index";
        if (!std::filesystem::exists(index_path)) {
            fmt::print("Nothing to clean (no index found)\n");
            return EXIT_SUCCESS;
        }

        auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };
        auto result = remove_indexed_outputs(index_path, mode);

        auto dirs_removed = remove_empty_directories(
            result.output_dirs, ctx.build_dir, ctx.root, mode);

        if (opts.dry_run)
            fmt::print("Would remove {} files, {} directories\n", result.removed_count, dirs_removed);
        else
            fmt::print("Removed {} files, {} directories\n", result.removed_count, dirs_removed);

        return result.error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    });
}

auto cmd_distclean(Options const& opts) -> int
{
    return with_clean_context(opts, [&](CleanContext& ctx) {
        auto index_path = ctx.build_dir / ".pup" / "index";
        auto error_count = std::size_t { 0 };
        auto output_dirs = std::set<std::filesystem::path> {};

        auto mode = OutputMode { .dry_run = opts.dry_run, .verbose = opts.verbose };

        if (std::filesystem::exists(index_path)) {
            auto result = remove_indexed_outputs(index_path, mode);
            error_count += result.error_count;
            output_dirs = std::move(result.output_dirs);
        }

        auto pup_dir = ctx.build_dir / ".pup";
        if (std::filesystem::exists(pup_dir)) {
            if (opts.dry_run) {
                fmt::print("Would remove: {}\n", pup_dir.string());
            } else {
                if (opts.verbose)
                    fmt::print("Removing: {}\n", pup_dir.string());
                std::filesystem::remove_all(pup_dir);
            }
        }

        auto config_path = ctx.build_dir / "tup.config";
        if (std::filesystem::exists(config_path)) {
            if (opts.dry_run) {
                fmt::print("Would remove: {}\n", config_path.string());
            } else {
                if (opts.verbose)
                    fmt::print("Removing: {}\n", config_path.string());
                std::filesystem::remove(config_path);
            }
        }

        output_dirs.insert(ctx.build_dir);
        remove_empty_directories(output_dirs, ctx.build_dir, ctx.root, mode);

        if (!opts.dry_run)
            fmt::print("Project reset complete\n");

        return error_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    });
}

} // namespace pup::cli
