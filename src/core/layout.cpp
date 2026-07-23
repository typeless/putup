// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/layout.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/print.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/platform/env.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace pup {

auto ProjectLayout::pup_dir() const -> StringId
{
    return path::join(global_pool().get(output_root), ".pup");
}

auto ProjectLayout::index_path() const -> StringId
{
    return path::join(global_pool().get(pup_dir()), "index");
}

auto ProjectLayout::resolve_source(std::string_view rel) const -> StringId
{
    return path::join(global_pool().get(source_root), rel);
}

auto ProjectLayout::resolve_config(std::string_view rel) const -> StringId
{
    return path::join(global_pool().get(config_root), rel);
}

auto ProjectLayout::resolve_output(std::string_view rel) const -> StringId
{
    return path::join(global_pool().get(output_root), rel);
}

namespace {

auto const PUP_SOURCE_DIR_ENV = "PUP_SOURCE_DIR";
auto const PUP_CONFIG_DIR_ENV = "PUP_CONFIG_DIR";
auto const PUP_BUILD_DIR_ENV = "PUP_BUILD_DIR";

auto get_env(char const* name) -> std::optional<std::string_view>
{
    if (auto const* value = pup::platform::get_env(name)) {
        if (*value != '\0') {
            return std::string_view { value };
        }
    }
    return std::nullopt;
}

auto is_build_dir(std::string_view dir) -> bool
{
    auto& pool = global_pool();
    return platform::exists(pool.get(path::join(dir, "tup.config")))
        || platform::is_directory(pool.get(path::join(dir, ".pup")));
}

} // namespace

auto find_build_subdir(
    std::string_view root
) -> std::optional<StringId>
{
    auto& pool = global_pool();
    for (auto const& name : { "build", "out", "variant" }) {
        auto dir = pool.get(path::join(root, name));
        if (is_build_dir(dir) && !foreign_build_dir_owner(dir, root)) {
            return pool.intern(dir);
        }
    }

    if (platform::is_directory(root)) {
        auto listing = platform::DirEntries {};
        if (platform::read_directory(root, listing)) {
            for (auto const& entry : listing.entries) {
                if (!entry.is_dir) {
                    continue;
                }
                auto entry_path = pool.get(path::join(root, entry.name));
                if (is_build_dir(entry_path) && !foreign_build_dir_owner(entry_path, root)) {
                    return pool.intern(entry_path);
                }
            }
        }
    }

    return std::nullopt;
}

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

auto record_build_dir_owner(ProjectLayout const& layout) -> void
{
    auto& pool = global_pool();
    auto output_root_sv = pool.get(layout.output_root);
    (void)platform::create_directories(output_root_sv);
    auto content = Buf {};
    content.fmt("{}\n", pool.get(path::relative(pool.get(layout.config_root), output_root_sv)));
    (void)platform::write_file(pool.get(path::join(output_root_sv, ".pup-project")), content.view());
}

auto foreign_build_dir_owner(
    std::string_view build_dir,
    std::string_view project_root
) -> std::optional<StringId>
{
    auto& pool = global_pool();
    auto stamp = Buf {};
    if (!platform::read_file(pool.get(path::join(build_dir, ".pup-project")), stamp)) {
        return std::nullopt;
    }
    auto rel = stamp.view();
    while (!rel.empty() && (rel.back() == '\n' || rel.back() == '\r')) {
        rel.remove_suffix(1);
    }
    if (rel.empty()) {
        return std::nullopt;
    }
    auto owner = path::is_absolute(rel)
        ? normalize_path(rel)
        : normalize_path(pool.get(path::join(build_dir, rel)));
    if (pool.get(owner) == pool.get(normalize_path(project_root))) {
        return std::nullopt;
    }
    return owner;
}

auto discover_variants(
    std::string_view source_root,
    std::string_view project_root
) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};

    if (!platform::is_directory(source_root)) {
        return result;
    }

    auto listing = platform::DirEntries {};
    if (!platform::read_directory(source_root, listing)) {
        return result;
    }

    for (auto const& entry : listing.entries) {
        if (!entry.is_dir) {
            continue;
        }
        auto entry_path = pool.get(path::join(source_root, entry.name));
        if (!is_build_dir(entry_path)) {
            continue;
        }
        if (auto owner = foreign_build_dir_owner(entry_path, project_root)) {
            eprint("Skipping {}: owned by another project ({})\n", entry.name, pool.get(*owner));
            continue;
        }
        result.push_back(pool.intern(entry.name));
    }

    std::sort(result.begin(), result.end());
    return result;
}

} // namespace pup
