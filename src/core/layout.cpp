// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/layout.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstdlib>

namespace pup {

namespace {

auto const PUP_SOURCE_DIR_ENV = "PUP_SOURCE_DIR";
auto const PUP_CONFIG_DIR_ENV = "PUP_CONFIG_DIR";
auto const PUP_BUILD_DIR_ENV = "PUP_BUILD_DIR";

auto get_env(char const* name) -> std::optional<String>
{
    if (auto const* value = std::getenv(name)) {
        if (*value != '\0') {
            return String { value };
        }
    }
    return std::nullopt;
}

auto find_build_subdir(
    std::string_view root
) -> std::optional<String>
{
    for (auto const& name : { "build", "out", "variant" }) {
        auto dir = path::join(root, name);
        if (platform::exists(path::join(dir, "tup.config"))
            || platform::is_directory(path::join(dir, ".pup"))) {
            return dir;
        }
    }

    if (platform::is_directory(root)) {
        auto entries = platform::read_directory(root);
        if (entries) {
            for (auto const& entry : *entries) {
                if (!entry.is_dir) {
                    continue;
                }
                auto entry_path = path::join(root, global_pool().get(entry.name));
                if (platform::exists(path::join(entry_path, "tup.config"))
                    || platform::is_directory(path::join(entry_path, ".pup"))) {
                    return entry_path;
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace

auto find_project_root(
    std::string_view start_dir
) -> std::optional<StringId>
{
    auto& pool = global_pool();
    auto current = String { start_dir };
    auto last_tupfile_dir = std::optional<StringId> {};

    while (true) {
        if (platform::exists(path::join(current, "Tupfile.ini"))) {
            return pool.intern(current);
        }

        if (platform::exists(path::join(current, "Tupfile"))) {
            last_tupfile_dir = pool.intern(current);
        }

        auto par = String { path::parent(current) };
        if (std::string_view { par } == std::string_view { current } || par.empty()) {
            return last_tupfile_dir;
        }
        current = std::move(par);
    }
}

auto normalize_path(std::string_view p) -> String
{
    auto result = platform::canonical(p);
    if (result) {
        return *result;
    }
    auto abs = platform::absolute(p);
    if (abs) {
        return *abs;
    }
    return String { p };
}

auto intern_path(std::string_view p) -> StringId
{
    return global_pool().intern(normalize_path(p));
}

auto discover_layout(LayoutOptions const& opts) -> Result<ProjectLayout>
{
    auto layout = ProjectLayout {};
    auto cwd = *platform::current_directory();

    auto& pool = global_pool();

    if (opts.source_dir) {
        auto src_sv = pool.get(*opts.source_dir);
        if (!platform::exists(src_sv)) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                String { "Source directory not found: " } + src_sv
            );
        }
        layout.source_root = intern_path(src_sv);
    } else if (auto env_source = get_env(PUP_SOURCE_DIR_ENV)) {
        if (!platform::exists(*env_source)) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                "PUP_SOURCE_DIR not found: " + *env_source
            );
        }
        layout.source_root = intern_path(*env_source);
    } else {
        auto root = find_project_root(cwd);
        if (!root) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                "Not in a pup/tup project (no Tupfile.ini found)"
            );
        }
        layout.source_root = intern_path(pool.get(*root));
    }

    if (opts.build_dir) {
        layout.output_root = intern_path(pool.get(*opts.build_dir));
    } else if (auto env_build = get_env(PUP_BUILD_DIR_ENV)) {
        layout.output_root = intern_path(*env_build);
    } else if (platform::exists(path::join(cwd, "tup.config")) && pool.intern(cwd) != layout.source_root) {
        layout.output_root = intern_path(cwd);
    } else if (auto build_subdir = find_build_subdir(pool.get(layout.source_root))) {
        layout.output_root = intern_path(*build_subdir);
    } else {
        layout.output_root = layout.source_root;
    }

    if (opts.config_dir) {
        auto cfg_sv = pool.get(*opts.config_dir);
        if (!platform::exists(cfg_sv)) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                String { "Config directory not found: " } + cfg_sv
            );
        }
        layout.config_root = intern_path(cfg_sv);
        if (!platform::exists(path::join(pool.get(layout.config_root), "Tupfile.ini"))) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                String { "Config directory does not contain Tupfile.ini: " } + pool.get(layout.config_root)
            );
        }
    } else if (auto env_config = get_env(PUP_CONFIG_DIR_ENV)) {
        if (!platform::exists(*env_config)) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                "PUP_CONFIG_DIR not found: " + *env_config
            );
        }
        layout.config_root = intern_path(*env_config);
        if (!platform::exists(path::join(pool.get(layout.config_root), "Tupfile.ini"))) {
            return make_error<ProjectLayout>(
                ErrorCode::NotFound,
                String { "Config directory does not contain Tupfile.ini: " } + pool.get(layout.config_root)
            );
        }
    } else if (platform::exists(path::join(pool.get(layout.source_root), "Tupfile.ini"))) {
        layout.config_root = layout.source_root;
    } else if (platform::exists(path::join(pool.get(layout.output_root), "Tupfile.ini"))) {
        layout.config_root = layout.output_root;
    } else {
        layout.config_root = layout.source_root;
    }

    return layout;
}

auto discover_variants(
    std::string_view source_root
) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};

    if (!platform::is_directory(source_root)) {
        return result;
    }

    auto entries = platform::read_directory(source_root);
    if (!entries) {
        return result;
    }

    for (auto const& entry : *entries) {
        if (!entry.is_dir) {
            continue;
        }
        auto name_sv = pool.get(entry.name);
        auto entry_path = path::join(source_root, name_sv);
        if (platform::exists(path::join(entry_path, "tup.config"))
            || platform::is_directory(path::join(entry_path, ".pup"))) {
            result.push_back(entry.name);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

} // namespace pup
