// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/gcc.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace pup::graph::scanners {

namespace {

/// Known compiler wrapper tools
auto is_compiler_wrapper(std::string_view name) -> bool
{
    return name == "ccache" || name == "distcc" || name == "sccache" || name == "icecc";
}

/// Check if a binary name is a recognized compiler (possibly cross-prefixed or version-suffixed).
/// Examples: gcc, g++, clang, x86_64-linux-gnu-gcc, arm-none-eabi-g++, gcc-15, clang++-18
auto is_compiler_name(std::string_view name) -> bool
{
    static constexpr std::string_view compilers[] = {
        "gcc",
        "g++",
        "cc",
        "c++",
        "clang",
        "clang++",
    };

    // Strip version suffix: gcc-15 → gcc, clang++-18 → clang++
    if (auto pos = name.rfind('-'); pos != std::string_view::npos) {
        auto suffix = name.substr(pos + 1);
        if (!suffix.empty()
            && std::ranges::all_of(suffix, [](char c) {
                   return std::isdigit(static_cast<unsigned char>(c)) || c == '.';
               })) {
            name = name.substr(0, pos);
        }
    }

    for (auto c : compilers) {
        if (name == c) {
            return true;
        }
        if (name.size() > c.size() + 1 && name[name.size() - c.size() - 1] == '-'
            && name.ends_with(c)) {
            return true;
        }
    }
    return false;
}

/// Check if a flag contains shell special characters that would cause issues
auto has_shell_special(std::string_view flag) -> bool
{
    return flag.find('`') != std::string_view::npos || flag.find("$(") != std::string_view::npos;
}

/// Check if a string needs shell quoting
auto needs_shell_quoting(std::string_view s) -> bool
{
    return std::ranges::any_of(s, [](char c) {
        return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\\' || c == '$' || c == '`'
            || c == '!' || c == '*' || c == '?' || c == '[' || c == ']' || c == '(' || c == ')'
            || c == '{' || c == '}' || c == '<' || c == '>' || c == '|' || c == '&' || c == ';'
            || c == '#' || c == '~';
    });
}

/// Quote a string for shell using single quotes (handles embedded single quotes)
auto shell_quote(std::string_view s) -> String
{
    if (!needs_shell_quoting(s)) {
        return String { s };
    }

    auto result = String { "'" };
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

/// Normalize a path lexically by removing foo/../ segments.
/// Unlike std::filesystem::lexically_normal(), works without filesystem access.
/// Needed because DEP commands run before output directories exist.
auto normalize_path_lexically(std::string_view path) -> String
{
    auto parts = Vec<String> {};
    auto start = std::size_t { 0 };
    auto is_absolute = !path.empty() && path[0] == '/';

    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        auto part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (part == ".." && !parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else {
                parts.push_back(String { part });
            }
        }
        start = end + 1;
    }

    if (parts.empty()) {
        return String { is_absolute ? "/" : "." };
    }

    auto result = String {};
    if (is_absolute) {
        result = String { "/" };
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
auto normalize_flag_path(std::string_view flag) -> String
{
    for (auto const* prefix : { "-I", "-isystem", "-iquote", "-include", "--sysroot=", "-isysroot" }) {
        if (flag.starts_with(prefix)) {
            auto path = flag.substr(std::strlen(prefix));
            if (!path.empty()) {
                return String { prefix } + normalize_path_lexically(path);
            }
        }
    }
    return String { flag };
}

/// Check if a flag is relevant for dependency generation
auto is_dep_relevant_flag(std::string_view flag) -> bool
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
auto is_source_file(std::string_view word) -> bool
{
    if (word.empty() || word[0] == '-') {
        return false;
    }
    auto dot_pos = word.rfind('.');
    if (dot_pos == std::string_view::npos) {
        return false;
    }
    auto ext = word.substr(dot_pos);
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".C" || ext == ".c++"
        || ext == ".m" || ext == ".mm"
        || ext == ".S" || ext == ".s" || ext == ".asm";
}

} // namespace

auto matches_gcc_compile(std::string_view command) -> bool
{
    auto word_ids = core::tokenize_shell_command(command);
    if (word_ids.empty()) {
        return false;
    }

    auto& pool = global_pool();
    auto compiler_idx = std::size_t { 0 };
    auto first_basename = pool.get(word_ids[0]);
    if (auto pos = first_basename.rfind('/'); pos != std::string_view::npos) {
        first_basename = first_basename.substr(pos + 1);
    }

    if (is_compiler_wrapper(first_basename) && word_ids.size() > 1) {
        compiler_idx = 1;
    }

    auto compiler_basename = pool.get(word_ids[compiler_idx]);
    if (auto pos = compiler_basename.rfind('/'); pos != std::string_view::npos) {
        compiler_basename = compiler_basename.substr(pos + 1);
    }

    if (!is_compiler_name(compiler_basename)) {
        return false;
    }

    for (auto i = compiler_idx + 1; i < word_ids.size(); ++i) {
        if (pool.get(word_ids[i]) == "-c") {
            return true;
        }
    }
    return false;
}

auto GccScanner::matches(CommandInfo const& cmd) const -> bool
{
    return matches_gcc_compile(global_pool().get(cmd.command));
}

auto GccScanner::has_dep_flags(std::string_view cmd) const -> bool
{
    auto pos = std::string_view::size_type { 0 };
    while ((pos = cmd.find("-M", pos)) != std::string_view::npos) {
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

auto GccScanner::build_dep_command(CommandInfo const& cmd) const -> std::optional<StringId>
{
    auto& pool = global_pool();
    auto word_ids = core::tokenize_shell_command(pool.get(cmd.command));
    if (word_ids.empty()) {
        return std::nullopt;
    }

    // Resolve all words to string_views up front
    auto words = Vec<std::string_view> {};
    words.reserve(word_ids.size());
    for (auto id : word_ids) {
        words.push_back(pool.get(id));
    }

    auto compiler_idx = std::size_t { 0 };
    auto first_basename = words[0];
    if (auto slash_pos = first_basename.rfind('/'); slash_pos != std::string_view::npos) {
        first_basename = first_basename.substr(slash_pos + 1);
    }

    if (is_compiler_wrapper(first_basename) && words.size() > 1) {
        compiler_idx = 1;
    }

    auto compiler_basename = words[compiler_idx];
    if (auto pos = compiler_basename.rfind('/'); pos != std::string_view::npos) {
        compiler_basename = compiler_basename.substr(pos + 1);
    }
    if (!is_compiler_name(compiler_basename)) {
        return std::nullopt;
    }

    auto dep_cmd = String {};

    for (auto i = std::size_t { 0 }; i <= compiler_idx; ++i) {
        if (i > 0) {
            dep_cmd += ' ';
        }
        dep_cmd += words[i];
    }

    dep_cmd += " -M";

    auto skip_next = false;
    auto source_files = Vec<std::string_view> {};
    for (auto i = compiler_idx + 1; i < words.size(); ++i) {
        if (skip_next) {
            dep_cmd += ' ';
            dep_cmd += shell_quote(normalize_path_lexically(words[i]));
            skip_next = false;
            continue;
        }

        auto w = words[i];

        if (w == "-c") {
            continue;
        }

        if (w == "-o") {
            ++i;
            continue;
        }

        if (is_dep_relevant_flag(w)) {
            dep_cmd += ' ';
            dep_cmd += shell_quote(normalize_flag_path(w));
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

    for (auto src : source_files) {
        dep_cmd += ' ';
        dep_cmd += shell_quote(src);
    }

    return pool.intern(dep_cmd);
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
