// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/gcc.hpp"

#include "pup/core/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>

namespace pup::graph::scanners {

namespace {

/// Known compiler wrapper tools
auto is_compiler_wrapper(std::string const& name) -> bool
{
    return name == "ccache" || name == "distcc" || name == "sccache" || name == "icecc";
}

/// Check if a flag contains shell special characters that would cause issues
auto has_shell_special(std::string const& flag) -> bool
{
    return flag.find('`') != std::string::npos || flag.find("$(") != std::string::npos;
}

/// Check if a string needs shell quoting
auto needs_shell_quoting(std::string const& s) -> bool
{
    return std::ranges::any_of(s, [](char c) {
        return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\\' || c == '$' || c == '`'
            || c == '!' || c == '*' || c == '?' || c == '[' || c == ']' || c == '(' || c == ')'
            || c == '{' || c == '}' || c == '<' || c == '>' || c == '|' || c == '&' || c == ';'
            || c == '#' || c == '~';
    });
}

/// Quote a string for shell using single quotes (handles embedded single quotes)
auto shell_quote(std::string const& s) -> std::string
{
    if (!needs_shell_quoting(s)) {
        return s;
    }

    auto result = std::string { "'" };
    for (auto c : s) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += '\'';
    return result;
}

/// Normalize a path by removing foo/../ segments.
/// Needed because DEP commands run before output directories exist.
auto normalize_path(std::string const& path) -> std::string
{
    auto parts = std::vector<std::string> {};
    auto start = std::size_t { 0 };
    auto is_absolute = !path.empty() && path[0] == '/';

    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        auto part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (part == ".." && !parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else {
                parts.push_back(std::move(part));
            }
        }
        start = end + 1;
    }

    if (parts.empty()) {
        return is_absolute ? "/" : ".";
    }

    auto result = std::string {};
    if (is_absolute) {
        result = "/";
    }
    for (auto const& part : parts) {
        if (!result.empty() && result.back() != '/') {
            result += '/';
        }
        result += part;
    }
    return result;
}

/// Normalize paths embedded in compiler flags
auto normalize_flag_path(std::string const& flag) -> std::string
{
    for (auto const* prefix : { "-I", "-isystem", "-iquote", "-include", "--sysroot=", "-isysroot" }) {
        if (flag.starts_with(prefix)) {
            auto path = flag.substr(std::strlen(prefix));
            if (!path.empty()) {
                return std::string { prefix } + normalize_path(path);
            }
        }
    }
    return flag;
}

/// Check if a flag is relevant for dependency generation
auto is_dep_relevant_flag(std::string const& flag) -> bool
{
    if (has_shell_special(flag)) {
        return false;
    }

    if (flag.starts_with("-I") || flag.starts_with("-isystem") || flag.starts_with("-iquote")) {
        return true;
    }
    if (flag.starts_with("-D") || flag.starts_with("-U")) {
        return true;
    }
    if (flag.starts_with("-std=")) {
        return true;
    }
    if (flag.starts_with("-include")) {
        return true;
    }
    if (flag.starts_with("--sysroot") || flag.starts_with("-isysroot")) {
        return true;
    }
    return false;
}

/// Check if a word looks like a source file
auto is_source_file(std::string const& word) -> bool
{
    if (word.empty() || word[0] == '-') {
        return false;
    }
    auto dot_pos = word.rfind('.');
    if (dot_pos == std::string::npos) {
        return false;
    }
    auto ext = word.substr(dot_pos);
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".C" || ext == ".c++"
        || ext == ".m" || ext == ".mm"
        || ext == ".S" || ext == ".s" || ext == ".asm";
}

/// Regex to match GCC/Clang compile commands
auto gcc_pattern() -> std::regex const&
{
    static auto const pattern = std::regex { R"((gcc|g\+\+|clang|clang\+\+|cc|c\+\+).*\s-c\s)" };
    return pattern;
}

} // namespace

auto GccScanner::matches(CommandInfo const& cmd) const -> bool
{
    return std::regex_search(cmd.command, gcc_pattern());
}

auto GccScanner::has_dep_flags(std::string const& cmd) const -> bool
{
    auto pos = std::string::size_type { 0 };
    while ((pos = cmd.find("-M", pos)) != std::string::npos) {
        if (pos > 0 && std::isspace(static_cast<unsigned char>(cmd[pos - 1])) == 0) {
            ++pos;
            continue;
        }
        auto next_pos = pos + 2;
        if (next_pos >= cmd.size()) {
            return true;
        }
        auto c = cmd[next_pos];
        if (c == 'D' || c == 'M' || c == 'F' || c == 'G' || c == 'P' || c == 'T' || c == 'Q' || c == 'V'
            || std::isspace(static_cast<unsigned char>(c)) != 0) {
            return true;
        }
        ++pos;
    }
    return false;
}

auto GccScanner::build_dep_command(CommandInfo const& cmd) const -> std::optional<std::string>
{
    auto words = core::tokenize_shell_command(cmd.command);
    if (words.empty()) {
        return std::nullopt;
    }

    auto compiler_idx = std::size_t { 0 };
    auto first_basename = words[0];
    if (auto slash_pos = first_basename.rfind('/'); slash_pos != std::string::npos) {
        first_basename = first_basename.substr(slash_pos + 1);
    }

    if (is_compiler_wrapper(first_basename) && words.size() > 1) {
        compiler_idx = 1;
    }

    auto dep_cmd = std::ostringstream {};

    for (auto i = std::size_t { 0 }; i <= compiler_idx; ++i) {
        if (i > 0) {
            dep_cmd << ' ';
        }
        dep_cmd << words[i];
    }

    dep_cmd << " -M";

    auto skip_next = false;
    auto source_files = std::vector<std::string> {};
    for (auto i = compiler_idx + 1; i < words.size(); ++i) {
        if (skip_next) {
            dep_cmd << ' ' << shell_quote(normalize_path(words[i]));
            skip_next = false;
            continue;
        }

        auto const& w = words[i];

        if (w == "-c") {
            continue;
        }

        if (w == "-o") {
            ++i;
            continue;
        }

        if (is_dep_relevant_flag(w)) {
            dep_cmd << ' ' << shell_quote(normalize_flag_path(w));
            if (w == "-I" || w == "-D" || w == "-U" || w == "-include"
                || w == "-isystem" || w == "-iquote" || w == "-isysroot") {
                skip_next = true;
            }
            continue;
        }

        if (is_source_file(w)) {
            source_files.push_back(w);
        }
    }

    for (auto const& src : source_files) {
        dep_cmd << ' ' << shell_quote(src);
    }

    return dep_cmd.str();
}

auto GccScanner::dep_spec() const -> DepSpec
{
    return DepSpec {
        .output_mode = DepOutputMode::Stdout,
    };
}

auto make_gcc_scanner() -> std::unique_ptr<DepScanner>
{
    return std::make_unique<GccScanner>();
}

} // namespace pup::graph::scanners
