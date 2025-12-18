// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/result.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto create_variant(
    std::filesystem::path const& root,
    std::string const& config_arg,
    std::optional<std::string> const& output_dir,
    bool verbose
) -> pup::Result<void>
{
    auto config_path = std::filesystem::path { config_arg };
    auto abs_config = std::filesystem::path { root / config_path };

    if (!std::filesystem::exists(abs_config)) {
        return pup::make_error<void>(
            pup::ErrorCode::IoError,
            fmt::format("Config file not found: {}", config_arg)
        );
    }

    auto variant_name = std::string {};
    if (output_dir) {
        variant_name = *output_dir;

        if (variant_name.empty()) {
            return pup::make_error<void>(
                pup::ErrorCode::InvalidArgument,
                "Output directory cannot be empty"
            );
        }

        auto test_path = std::filesystem::path { variant_name };
        if (test_path.is_absolute()) {
            return pup::make_error<void>(
                pup::ErrorCode::InvalidArgument,
                "Output directory must be relative to project root"
            );
        }

        for (auto const& part : test_path) {
            if (part == "..") {
                return pup::make_error<void>(
                    pup::ErrorCode::InvalidArgument,
                    "Output directory cannot contain '..' components"
                );
            }
        }
    } else {
        auto stem = std::string { config_path.stem().string() };
        if (stem.empty()) {
            return pup::make_error<void>(
                pup::ErrorCode::ParseError,
                fmt::format("Invalid config filename: {}", config_arg)
            );
        }
        variant_name = "build-" + stem;
    }
    auto variant_dir = std::filesystem::path { root / variant_name };

    if (!std::filesystem::exists(variant_dir)) {
        auto ec = std::error_code {};
        std::filesystem::create_directories(variant_dir, ec);
        if (ec) {
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to create {}: {}", variant_name, ec.message())
            );
        }
    }

    auto tup_config = std::filesystem::path { variant_dir / "tup.config" };

    if (std::filesystem::exists(tup_config)) {
        auto ec = std::error_code {};
        std::filesystem::remove(tup_config, ec);
    }

    auto display_target = std::filesystem::path {};

#ifdef _WIN32
    {
        auto ec = std::error_code {};
        std::filesystem::copy_file(abs_config, tup_config, ec);
        if (ec) {
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to copy config: {}", ec.message())
            );
        }
        display_target = abs_config;
    }
#else
    {
        auto ec = std::error_code {};
        auto rel_config = std::filesystem::relative(abs_config, variant_dir, ec);
        if (ec) {
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to compute relative path: {}", ec.message())
            );
        }

        std::filesystem::create_symlink(rel_config, tup_config, ec);
        if (ec) {
            return pup::make_error<void>(
                pup::ErrorCode::IoError,
                fmt::format("Failed to create symlink: {}", ec.message())
            );
        }
        display_target = rel_config;
    }
#endif

    fmt::print("Created variant '{}'\n", variant_name);
    if (verbose) {
        fmt::print("  {} -> {}\n", tup_config.string(), display_target.string());
    }

    return {};
}

} // namespace

auto cmd_variant(Options const& opts) -> int
{
    if (opts.targets.empty()) {
        fmt::print(stderr, "Error: No config file specified\n");
        fmt::print(stderr, "Usage: pup variant <config> [output_dir]\n");
        return EXIT_FAILURE;
    }

    auto root = std::optional<std::filesystem::path> { pup::find_project_root(std::filesystem::current_path()) };
    if (!root) {
        fmt::print(stderr, "Error: Not in a pup/tup project\n");
        fmt::print(stderr, "Run 'pup init' first\n");
        return EXIT_FAILURE;
    }

    auto config_path = opts.targets[0];
    auto output_dir = std::optional<std::string> {};
    if (opts.targets.size() > 1) {
        output_dir = opts.targets[1];
    }

    auto result = pup::Result<void> { create_variant(*root, config_path, output_dir, opts.verbose) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace pup::cli
