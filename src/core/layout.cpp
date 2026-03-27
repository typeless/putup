// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/layout.hpp"
#include "pup/core/buf.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstdlib>

namespace pup {

namespace {

auto const PUP_SOURCE_DIR_ENV = "PUP_SOURCE_DIR";
auto const PUP_CONFIG_DIR_ENV = "PUP_CONFIG_DIR";
auto const PUP_BUILD_DIR_ENV = "PUP_BUILD_DIR";

auto get_env(char const* name) -> std::optional<std::string_view>
{
    if (auto const* value = std::getenv(name)) {
        if (*value != '\0') {
            return std::string_view { value };
        }
    }
    return std::nullopt;
}

auto find_build_subdir(
    std::string_view root
) -> std::optional<StringId>
{
    auto& pool = global_pool();
    for (auto const& name : { "build", "out", "variant" }) {
        auto dir = pool.get(path::join(root, name));
        if (platform::exists(pool.get(path::join(dir, "tup.config")))
            || platform::is_directory(pool.get(path::join(dir, ".pup")))) {
            return pool.intern(dir);
        }
    }

    if (platform::is_directory(root)) {
        auto entries = platform::read_directory(root);
        if (entries) {
            for (auto const& entry : *entries) {
                if (!entry.is_dir) {
                    continue;
                }
                auto entry_path = pool.get(path::join(root, pool.get(entry.name)));
                if (platform::exists(pool.get(path::join(entry_path, "tup.config")))
                    || platform::is_directory(pool.get(path::join(entry_path, ".pup")))) {
                    return pool.intern(entry_path);
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
    auto current = pool.intern(start_dir);
    auto last_tupfile_dir = std::optional<StringId> {};

    while (true) {
        auto current_sv = pool.get(current);
        if (platform::exists(pool.get(path::join(current_sv, "Tupfile.ini")))) {
            return current;
        }

        if (platform::exists(pool.get(path::join(current_sv, "Tupfile")))) {
            last_tupfile_dir = current;
        }

        auto par_sv = path::parent(current_sv);
        if (par_sv == current_sv || par_sv.empty()) {
            return last_tupfile_dir;
        }
        current = pool.intern(par_sv);
    }
}

auto normalize_path(std::string_view p) -> StringId
{
    auto result = platform::canonical(p);
    if (result) {
        return *result;
    }
    auto abs = platform::absolute(p);
    if (abs) {
        return *abs;
    }
    return global_pool().intern(p);
}

auto intern_path(std::string_view p) -> StringId
{
    return normalize_path(p);
}

auto discover_layout(LayoutOptions const& opts) -> Result<ProjectLayout>
{
    auto layout = ProjectLayout {};
    auto cwd_id = *platform::current_directory();
    auto cwd = global_pool().get(cwd_id);

    auto& pool = global_pool();

    if (opts.source_dir) {
        auto src_sv = pool.get(*opts.source_dir);
        if (!platform::exists(src_sv)) {
            auto err = Buf {};
            err.fmt("Source directory not found: {}", src_sv);
            return make_error<ProjectLayout>(ErrorCode::NotFound, err.view());
        }
        layout.source_root = intern_path(src_sv);
    } else if (auto env_source = get_env(PUP_SOURCE_DIR_ENV)) {
        if (!platform::exists(*env_source)) {
            auto err = Buf {};
            err.fmt("PUP_SOURCE_DIR not found: {}", *env_source);
            return make_error<ProjectLayout>(ErrorCode::NotFound, err.view());
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
    } else if (platform::exists(pool.get(path::join(cwd, "tup.config"))) && pool.intern(cwd) != layout.source_root) {
        layout.output_root = intern_path(cwd);
    } else if (auto build_subdir = find_build_subdir(pool.get(layout.source_root))) {
        layout.output_root = intern_path(pool.get(*build_subdir));
    } else {
        layout.output_root = layout.source_root;
    }

    if (opts.config_dir) {
        auto cfg_sv = pool.get(*opts.config_dir);
        if (!platform::exists(cfg_sv)) {
            auto err = Buf {};
            err.fmt("Config directory not found: {}", cfg_sv);
            return make_error<ProjectLayout>(ErrorCode::NotFound, err.view());
        }
        layout.config_root = intern_path(cfg_sv);
        if (!platform::exists(pool.get(path::join(pool.get(layout.config_root), "Tupfile.ini")))) {
            auto err = Buf {};
            err.fmt("Config directory does not contain Tupfile.ini: {}", pool.get(layout.config_root));
            return make_error<ProjectLayout>(ErrorCode::NotFound, err.view());
        }
    } else if (auto env_config = get_env(PUP_CONFIG_DIR_ENV)) {
        if (!platform::exists(*env_config)) {
            auto err = Buf {};
            err.fmt("PUP_CONFIG_DIR not found: {}", *env_config);
            return make_error<ProjectLayout>(ErrorCode::NotFound, err.view());
        }
        layout.config_root = intern_path(*env_config);
        if (!platform::exists(pool.get(path::join(pool.get(layout.config_root), "Tupfile.ini")))) {
            auto err = Buf {};
            err.fmt("Config directory does not contain Tupfile.ini: {}", pool.get(layout.config_root));
            return make_error<ProjectLayout>(ErrorCode::NotFound, err.view());
        }
    } else if (platform::exists(pool.get(path::join(pool.get(layout.source_root), "Tupfile.ini")))) {
        layout.config_root = layout.source_root;
    } else if (platform::exists(pool.get(path::join(pool.get(layout.output_root), "Tupfile.ini")))) {
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
        auto entry_path = pool.get(path::join(source_root, name_sv));
        if (platform::exists(pool.get(path::join(entry_path, "tup.config")))
            || platform::is_directory(pool.get(path::join(entry_path, ".pup")))) {
            result.push_back(entry.name);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

} // namespace pup
