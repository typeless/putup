// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/core/layout.hpp"

#include <cstdlib>

namespace pup {

namespace {

auto const PUP_SOURCE_DIR_ENV = "PUP_SOURCE_DIR";
auto const PUP_BUILD_DIR_ENV = "PUP_BUILD_DIR";

auto get_env(char const* name) -> std::optional<std::filesystem::path>
{
    if (auto const* value = std::getenv(name)) {
        if (*value != '\0') {
            return std::filesystem::path { value };
        }
    }
    return std::nullopt;
}

auto find_build_subdir(
    std::filesystem::path const& root) -> std::optional<std::filesystem::path>
{
    for (auto const& name : { "build", "out", "variant" }) {
        auto dir = std::filesystem::path { root / name };
        if (std::filesystem::exists(dir / "tup.config")
            || std::filesystem::is_directory(dir / ".pup")) {
            return dir;
        }
    }

    if (std::filesystem::is_directory(root)) {
        for (auto const& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory()) {
                if (std::filesystem::exists(entry.path() / "tup.config")
                    || std::filesystem::is_directory(entry.path() / ".pup")) {
                    return entry.path();
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace

auto find_project_root(
    std::filesystem::path const& start_dir) -> std::optional<std::filesystem::path>
{
    auto current = std::filesystem::path { start_dir };
    auto last_tupfile_dir = std::optional<std::filesystem::path> {};

    while (true) {
        // Tupfile.ini is the authoritative project root marker.
        if (std::filesystem::exists(current / "Tupfile.ini")) {
            return current;
        }

        // Track the topmost directory with a Tupfile (fallback for simple projects)
        if (std::filesystem::exists(current / "Tupfile")) {
            last_tupfile_dir = current;
        }

        auto parent = std::filesystem::path { current.parent_path() };
        if (parent == current) {
            // Reached filesystem root. Use the topmost Tupfile dir if found.
            return last_tupfile_dir;
        }
        current = parent;
    }
}

/// Normalize a path: make absolute and resolve symlinks where possible.
/// Uses weakly_canonical which doesn't require the path to fully exist.
auto normalize_path(std::filesystem::path const& path) -> std::filesystem::path
{
    auto ec = std::error_code {};
    auto result = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return std::filesystem::absolute(path);
    }
    // Ensure result is absolute (weakly_canonical may return relative for non-existent paths)
    if (!result.is_absolute()) {
        result = std::filesystem::absolute(result);
    }
    return result;
}

auto discover_layout(LayoutOptions const& opts) -> Result<ProjectLayout>
{
    auto layout = ProjectLayout {};
    auto cwd = std::filesystem::path { std::filesystem::current_path() };

    // Step 1: Determine source_root
    // Priority: CLI arg > env var > walk up from cwd
    if (opts.source_dir) {
        if (!std::filesystem::exists(*opts.source_dir)) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                "Source directory not found: " + opts.source_dir->string());
        }
        layout.source_root = normalize_path(*opts.source_dir);
    } else if (auto env_source = get_env(PUP_SOURCE_DIR_ENV)) {
        if (!std::filesystem::exists(*env_source)) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                "PUP_SOURCE_DIR not found: " + env_source->string());
        }
        layout.source_root = normalize_path(*env_source);
    } else {
        auto root = find_project_root(cwd);
        if (!root) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                "Not in a pup/tup project (no Tupfile.ini found)");
        }
        layout.source_root = normalize_path(*root);
    }

    // Validate source_root has Tupfile.ini or Tupfile
    if (!std::filesystem::exists(layout.source_root / "Tupfile.ini")
        && !std::filesystem::exists(layout.source_root / "Tupfile")) {
        return make_error<ProjectLayout>(
            ErrorCode::NotFound,
            "Source directory does not contain Tupfile.ini: " + layout.source_root.string());
    }

    // Step 2: Determine output_root (where outputs/.pup/tup.config go)
    // Priority: CLI arg > env var > cwd if has tup.config > variant subdir > source_root
    if (opts.build_dir) {
        layout.output_root = normalize_path(*opts.build_dir);
    } else if (auto env_build = get_env(PUP_BUILD_DIR_ENV)) {
        layout.output_root = normalize_path(*env_build);
    } else if (std::filesystem::exists(cwd / "tup.config") && cwd != layout.source_root) {
        // cwd contains tup.config and is not source_root - out-of-tree build
        layout.output_root = normalize_path(cwd);
    } else if (auto build_subdir = find_build_subdir(layout.source_root)) {
        // Found subdirectory with tup.config
        layout.output_root = normalize_path(*build_subdir);
    } else {
        // In-tree build: outputs go to source_root
        layout.output_root = layout.source_root;
    }

    return layout;
}

} // namespace pup
