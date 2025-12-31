// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/target.hpp"

#include <algorithm>
#include <set>

namespace fs = std::filesystem;

namespace pup {

namespace {

auto is_source_file(fs::path const& path) -> bool
{
    auto const ext = path.extension().string();
    static auto const source_exts = std::set<std::string> {
        ".c", ".cc", ".cpp", ".cxx", ".C", ".h", ".hh", ".hpp", ".hxx", ".H", ".s", ".S", ".asm"
    };
    return source_exts.contains(ext);
}

auto is_variant_dir(fs::path const& dir) -> bool
{
    auto config_path = dir / "tup.config";
    return fs::exists(config_path);
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

} // namespace

auto parse_target(
    fs::path const& project_root,
    std::string const& target_path
) -> Result<Target>
{
    if (target_path.empty()) {
        return unexpected<Error> { Error { ErrorCode::InvalidArgument, "empty target path" } };
    }

    auto full_path = project_root / target_path;
    auto target = Target {};
    auto path = fs::path { target_path };
    auto first_component = *path.begin();

    auto variant_path = project_root / first_component;
    if (is_variant_dir(variant_path)) {
        target.variant = first_component;

        auto remainder = fs::path {};
        auto it = path.begin();
        ++it;
        for (; it != path.end(); ++it) {
            remainder /= *it;
        }

        target.scope_or_output = remainder;
        full_path = variant_path / remainder;
    } else {
        target.scope_or_output = path;
    }

    if (fs::exists(full_path)) {
        if (fs::is_regular_file(full_path)) {
            if (is_source_file(full_path)) {
                return unexpected<Error> { Error { ErrorCode::InvalidArgument, "source file, not build output: " + target_path } };
            }

            target.is_output = true;
        }
    } else {
        // Path doesn't exist - check if parent exists (output target for from-scratch build)
        auto parent = full_path.parent_path();
        if (parent.empty()) {
            parent = project_root;
        }
        if (!fs::exists(parent)) {
            return unexpected<Error> { Error { ErrorCode::NotFound, "path not found: " + target_path } };
        }

        // Reject source file extensions even for non-existent files
        if (is_source_file(full_path)) {
            return unexpected<Error> { Error { ErrorCode::InvalidArgument, "source file, not build output: " + target_path } };
        }

        // Parent exists - assume output file (validate in cmd_build.cpp after graph is built)
        target.is_output = true;
    }

    return target;
}

auto expand_glob_target(
    fs::path const& project_root,
    std::string const& pattern
) -> std::vector<Target>
{
    auto result = std::vector<Target> {};
    auto path = fs::path { pattern };

    if (path.empty()) {
        return result;
    }

    auto first_component = path.begin()->string();
    auto has_glob = first_component.find('*') != std::string::npos;

    if (!has_glob) {
        auto parsed = parse_target(project_root, pattern);
        if (parsed.has_value()) {
            result.push_back(*parsed);
        }
        return result;
    }

    auto remainder = fs::path {};
    auto it = path.begin();
    ++it;
    for (; it != path.end(); ++it) {
        remainder /= *it;
    }

    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(project_root, ec)) {
        if (!entry.is_directory()) {
            continue;
        }

        auto name = entry.path().filename().string();
        if (!fnmatch_simple(first_component, name)) {
            continue;
        }

        if (!is_variant_dir(entry.path())) {
            continue;
        }

        auto target = Target {};
        target.variant = entry.path().filename();

        if (!remainder.empty()) {
            auto full_path = entry.path() / remainder;
            target.scope_or_output = remainder;
            if (fs::is_regular_file(full_path)) {
                target.is_output = true;
            } else if (!fs::is_directory(full_path) && !is_source_file(full_path)) {
                // Non-existent file that's not a source - assume output (validate later)
                auto parent = full_path.parent_path();
                if (!parent.empty() && fs::exists(parent)) {
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
    fs::path const& project_root,
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
