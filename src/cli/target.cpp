// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/target.hpp"
#include "pup/core/path.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <string_view>

namespace pup {

namespace {

auto is_source_file(std::string const& p) -> bool
{
    auto ext = pup::path::extension(p);
    // Sorted at compile time for binary_search
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
    return std::binary_search(std::begin(source_exts), std::end(source_exts), ext);
}

auto is_variant_dir(std::string const& dir) -> bool
{
    return pup::platform::exists(pup::path::join(dir, "tup.config"));
}

auto fnmatch_simple(std::string const& pattern, std::string const& name) -> bool
{
    auto star_pos = pattern.find('*');
    if (star_pos == std::string::npos) {
        return pattern == name;
    }

    auto prefix = pattern.substr(0, star_pos);
    auto suffix = pattern.substr(star_pos + 1);

    if (name.size() < prefix.size() + suffix.size()) {
        return false;
    }

    return name.starts_with(prefix) && name.ends_with(suffix);
}

auto split_first_component(std::string const& p) -> std::pair<std::string, std::string>
{
    auto slash = p.find('/');
    if (slash == std::string::npos) {
        return { p, {} };
    }
    return { p.substr(0, slash), p.substr(slash + 1) };
}

} // namespace

auto parse_target(
    std::string const& project_root,
    std::string const& target_path
) -> Result<Target>
{
    if (target_path.empty()) {
        return unexpected<Error> { Error { ErrorCode::InvalidArgument, "empty target path" } };
    }

    auto full_path = pup::path::join(project_root, target_path);
    auto target = Target {};

    auto [first_component, remainder] = split_first_component(target_path);

    auto variant_path = pup::path::join(project_root, first_component);
    if (is_variant_dir(variant_path)) {
        target.variant = first_component;
        target.scope_or_output = remainder;
        full_path = pup::path::join(variant_path, remainder);
    } else {
        target.scope_or_output = target_path;
    }

    if (pup::platform::exists(full_path)) {
        if (pup::platform::is_file(full_path)) {
            if (is_source_file(full_path)) {
                return unexpected<Error> { Error { ErrorCode::InvalidArgument, "source file, not build output: " + target_path } };
            }
            target.is_output = true;
        }
    } else {
        auto par = std::string { pup::path::parent(full_path) };
        if (par.empty()) {
            par = project_root;
        }
        if (!pup::platform::exists(par)) {
            return unexpected<Error> { Error { ErrorCode::NotFound, "path not found: " + target_path } };
        }

        if (is_source_file(full_path)) {
            return unexpected<Error> { Error { ErrorCode::InvalidArgument, "source file, not build output: " + target_path } };
        }

        target.is_output = true;
    }

    return target;
}

auto expand_glob_target(
    std::string const& project_root,
    std::string const& pattern
) -> std::vector<Target>
{
    auto result = std::vector<Target> {};

    if (pattern.empty()) {
        return result;
    }

    auto [first_component, remainder] = split_first_component(pattern);
    auto has_glob = first_component.find('*') != std::string::npos;

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

        if (!fnmatch_simple(first_component, entry.name)) {
            continue;
        }

        auto entry_path = pup::path::join(project_root, entry.name);
        if (!is_variant_dir(entry_path)) {
            continue;
        }

        auto target = Target {};
        target.variant = entry.name;

        if (!remainder.empty()) {
            auto full_path = pup::path::join(entry_path, remainder);
            target.scope_or_output = remainder;
            if (pup::platform::is_file(full_path)) {
                target.is_output = true;
            } else if (!pup::platform::is_directory(full_path) && !is_source_file(full_path)) {
                auto par = std::string { pup::path::parent(full_path) };
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

auto is_glob_pattern(std::string const& s) -> bool
{
    return s.find('*') != std::string::npos;
}

} // namespace

auto validate_target_consistency(
    std::string const& project_root,
    std::vector<std::string> const& targets
) -> Result<std::vector<Target>>
{
    auto result = std::vector<Target> {};
    auto has_variant = std::optional<bool> {};

    for (auto const& target_str : targets) {
        auto parsed_targets = std::vector<Target> {};

        if (is_glob_pattern(target_str)) {
            parsed_targets = expand_glob_target(project_root, target_str);
            if (parsed_targets.empty()) {
                return unexpected<Error> { Error { ErrorCode::NotFound, "no variants match pattern: " + target_str } };
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
                return unexpected<Error> { Error { ErrorCode::InvalidArgument, "cannot mix variant-specific and all-variant targets" } };
            }

            result.push_back(target);
        }
    }

    return result;
}

} // namespace pup
