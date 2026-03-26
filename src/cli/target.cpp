// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/target.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <string_view>

namespace pup {

namespace {

auto is_source_file(std::string_view p) -> bool
{
    auto ext = pup::path::extension(p);
    static constexpr std::string_view source_exts[] = {
        ".C",
        ".H",
        ".S",
        ".asm",
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".h",
        ".hh",
        ".hpp",
        ".hxx",
        ".s",
    };
    static_assert(std::is_sorted(std::begin(source_exts), std::end(source_exts)));
    return std::binary_search(std::begin(source_exts), std::end(source_exts), ext);
}

auto is_variant_dir(std::string_view dir) -> bool
{
    return pup::platform::exists(pup::path::join(dir, "tup.config"));
}

auto fnmatch_simple(std::string_view pattern, std::string_view name) -> bool
{
    auto star_pos = pattern.find('*');
    if (star_pos == std::string_view::npos) {
        return pattern == name;
    }

    auto prefix = pattern.substr(0, star_pos);
    auto suffix = pattern.substr(star_pos + 1);

    if (name.size() < prefix.size() + suffix.size()) {
        return false;
    }

    return name.starts_with(prefix) && name.ends_with(suffix);
}

auto split_first_component(std::string_view p) -> std::pair<String, String>
{
    auto slash = p.find('/');
    if (slash == std::string_view::npos) {
        return { String { p }, {} };
    }
    return { String { p.substr(0, slash) }, String { p.substr(slash + 1) } };
}

} // namespace

auto parse_target(
    std::string_view project_root,
    std::string_view target_path
) -> Result<Target>
{
    auto& pool = global_pool();

    if (target_path.empty()) {
        return make_error<Target>(ErrorCode::InvalidArgument, "empty target path");
    }

    auto full_path = pup::path::join(project_root, target_path);
    auto target = Target {};

    auto [first_component, remainder] = split_first_component(target_path);

    auto variant_path = pup::path::join(project_root, first_component);
    if (is_variant_dir(variant_path)) {
        target.variant = pool.intern(first_component);
        target.scope_or_output = pool.intern(remainder);
        full_path = pup::path::join(variant_path, remainder);
    } else {
        target.scope_or_output = pool.intern(target_path);
    }

    if (pup::platform::exists(full_path)) {
        if (pup::platform::is_file(full_path)) {
            if (is_source_file(full_path)) {
                return make_error<Target>(ErrorCode::InvalidArgument, String { "source file, not build output: " } + target_path);
            }
            target.is_output = true;
        }
    } else {
        auto par = String { pup::path::parent(full_path) };
        if (par.empty()) {
            par = project_root;
        }
        if (!pup::platform::exists(par)) {
            return make_error<Target>(ErrorCode::NotFound, String { "path not found: " } + target_path);
        }

        if (is_source_file(full_path)) {
            return make_error<Target>(ErrorCode::InvalidArgument, String { "source file, not build output: " } + target_path);
        }

        target.is_output = true;
    }

    return target;
}

auto expand_glob_target(
    std::string_view project_root,
    std::string_view pattern
) -> Vec<Target>
{
    auto& pool = global_pool();
    auto result = Vec<Target> {};

    if (pattern.empty()) {
        return result;
    }

    auto [first_component, remainder] = split_first_component(pattern);
    auto has_glob = first_component.find('*') != String::npos;

    if (!has_glob) {
        auto parsed = parse_target(project_root, pattern);
        if (parsed.has_value()) {
            result.push_back(*parsed);
        }
        return result;
    }

    auto entries = pup::platform::read_directory(project_root);
    if (!entries) {
        return result;
    }

    for (auto const& entry : *entries) {
        if (!entry.is_dir) {
            continue;
        }

        auto name_sv = pool.get(entry.name);
        if (!fnmatch_simple(first_component, name_sv)) {
            continue;
        }

        auto entry_path = pup::path::join(project_root, name_sv);
        if (!is_variant_dir(entry_path)) {
            continue;
        }

        auto target = Target {};
        target.variant = entry.name;

        if (!remainder.empty()) {
            auto full_path = pup::path::join(entry_path, remainder);
            target.scope_or_output = pool.intern(remainder);
            if (pup::platform::is_file(full_path)) {
                target.is_output = true;
            } else if (!pup::platform::is_directory(full_path) && !is_source_file(full_path)) {
                auto par = String { pup::path::parent(full_path) };
                if (!par.empty() && pup::platform::exists(par)) {
                    target.is_output = true;
                }
            }
        }

        result.push_back(target);
    }

    return result;
}

namespace {

auto is_glob_pattern(std::string_view s) -> bool
{
    return s.find('*') != std::string_view::npos;
}

} // namespace

auto validate_target_consistency(
    std::string_view project_root,
    Vec<StringId> const& targets
) -> Result<Vec<Target>>
{
    auto& pool = global_pool();
    auto result = Vec<Target> {};
    auto has_variant = std::optional<bool> {};

    for (auto target_id : targets) {
        auto target_str = pool.get(target_id);
        auto parsed_targets = Vec<Target> {};

        if (is_glob_pattern(target_str)) {
            parsed_targets = expand_glob_target(project_root, target_str);
            if (parsed_targets.empty()) {
                return make_error<Vec<Target>>(ErrorCode::NotFound, String { "no variants match pattern: " } + target_str);
            }
        } else {
            auto parsed = parse_target(project_root, target_str);
            if (!parsed.has_value()) {
                return unexpected<Error> { parsed.error() };
            }
            parsed_targets.push_back(*parsed);
        }

        for (auto const& target : parsed_targets) {
            auto const this_has_variant = target.variant.has_value();

            if (!has_variant.has_value()) {
                has_variant = this_has_variant;
            } else if (*has_variant != this_has_variant) {
                return make_error<Vec<Target>>(ErrorCode::InvalidArgument, "cannot mix variant-specific and all-variant targets");
            }

            result.push_back(target);
        }
    }

    return result;
}

} // namespace pup
