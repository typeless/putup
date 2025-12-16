// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/rule_pattern.hpp"

#include "pup/core/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fmt/core.h>
#include <sstream>

namespace pup::graph {

namespace {

/// Check if command already has dependency generation flags
auto has_dep_flags(std::string const& cmd) -> bool
{
    // Check for -M* flags that generate dependency info
    // -MD, -MMD produce .d files alongside compilation
    // -M, -MM produce deps to stdout (stop compilation)
    // -MF specifies output file
    auto pos = std::string::size_type { 0 };
    while ((pos = cmd.find("-M", pos)) != std::string::npos) {
        // Check it's at start or preceded by whitespace
        if (pos > 0 && std::isspace(static_cast<unsigned char>(cmd[pos - 1])) == 0) {
            ++pos;
            continue;
        }
        // Check the flag type
        auto next_pos = pos + 2;
        // Bare -M at end of string
        if (next_pos >= cmd.size()) {
            return true;
        }
        auto c = cmd[next_pos];
        // -MD, -MM, -MMD, -MF, -MG, -MP, -MT, -MQ, -MV, or bare -M followed by whitespace
        if (c == 'D' || c == 'M' || c == 'F' || c == 'G' || c == 'P' || c == 'T' || c == 'Q' || c == 'V'
            || std::isspace(static_cast<unsigned char>(c)) != 0) {
            return true;
        }
        ++pos;
    }
    return false;
}

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

    // Use single quotes, escaping embedded single quotes as: '\''
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

/// Normalize a path by removing foo/../ segments
/// This is needed because DEP commands run before output directories exist,
/// so paths like build-dir/foo/bar/../../../include can't be resolved if
/// build-dir/foo/bar/ doesn't exist yet.
auto normalize_path(std::string const& path) -> std::string
{
    auto parts = std::vector<std::string> {};
    auto start = std::size_t { 0 };
    auto is_absolute = !path.empty() && path[0] == '/';

    // Split by /
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

    // Rebuild path (preserve "." for current directory if nothing else)
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
/// Handles: -I<path>, -isystem<path>, -iquote<path>, -include<path>, -D, -U, -std=, --sysroot=<path>
auto normalize_flag_path(std::string const& flag) -> std::string
{
    // Flags with path immediately after
    for (auto const* prefix : { "-I", "-isystem", "-iquote", "-include", "--sysroot=", "-isysroot" }) {
        if (flag.starts_with(prefix)) {
            auto path = flag.substr(std::strlen(prefix));
            if (!path.empty()) {
                return std::string { prefix } + normalize_path(path);
            }
        }
    }
    // Flags without paths or that shouldn't be normalized
    return flag;
}

/// Check if a flag is relevant for dependency generation
auto is_dep_relevant_flag(std::string const& flag) -> bool
{
    // Skip flags with shell command substitution (backticks or $())
    if (has_shell_special(flag)) {
        return false;
    }

    // Include paths
    if (flag.starts_with("-I") || flag.starts_with("-isystem") || flag.starts_with("-iquote")) {
        return true;
    }
    // Preprocessor defines
    if (flag.starts_with("-D") || flag.starts_with("-U")) {
        return true;
    }
    // Language standard
    if (flag.starts_with("-std=")) {
        return true;
    }
    // Force include - can be "-include path" or "-includepath" (GCC accepts both)
    if (flag.starts_with("-include")) {
        return true;
    }
    // Sysroot
    if (flag.starts_with("--sysroot") || flag.starts_with("-isysroot")) {
        return true;
    }
    return false;
}

/// Check if a word looks like a source file (not a flag)
auto is_source_file(std::string const& word) -> bool
{
    if (word.empty() || word[0] == '-') {
        return false;
    }
    // Check for common source file extensions
    auto dot_pos = word.rfind('.');
    if (dot_pos == std::string::npos) {
        return false;
    }
    auto ext = word.substr(dot_pos);
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".C" || ext == ".c++"
        || ext == ".S" || ext == ".s" || ext == ".asm";
}

/// Build a dep-scan command from the original compile command
auto build_dep_scan_command(CommandInfo const& cmd) -> std::string
{
    // Parse the original command to extract compiler, flags, and source files
    // Command format: "gcc -c foo.c -o foo.o" or "ccache gcc -c foo.c -o foo.o"
    auto words = core::tokenize_shell_command(cmd.command);

    if (words.empty()) {
        return {};
    }

    // Find compiler (possibly after a wrapper like ccache)
    auto compiler_idx = std::size_t { 0 };
    auto first_basename = words[0];
    if (auto slash_pos = first_basename.rfind('/'); slash_pos != std::string::npos) {
        first_basename = first_basename.substr(slash_pos + 1);
    }

    if (is_compiler_wrapper(first_basename) && words.size() > 1) {
        compiler_idx = 1;
    }

    // Build dep command: wrapper? compiler -M flags inputs...
    auto dep_cmd = std::ostringstream {};

    // Add wrapper and compiler
    for (auto i = std::size_t { 0 }; i <= compiler_idx; ++i) {
        if (i > 0) {
            dep_cmd << ' ';
        }
        dep_cmd << words[i];
    }

    // Add -M flag for dependency output
    dep_cmd << " -M";

    // Extract relevant flags and source files from original command
    auto skip_next = false;
    auto source_files = std::vector<std::string> {};
    for (auto i = compiler_idx + 1; i < words.size(); ++i) {
        if (skip_next) {
            // This is the argument to a flag like -include (normalize the path)
            dep_cmd << ' ' << shell_quote(normalize_path(words[i]));
            skip_next = false;
            continue;
        }

        auto const& w = words[i];

        // Skip -c flag
        if (w == "-c") {
            continue;
        }

        // Skip -o and its argument
        if (w == "-o") {
            ++i; // Skip the output file argument
            continue;
        }

        // Include relevant preprocessor flags (normalize embedded paths)
        if (is_dep_relevant_flag(w)) {
            dep_cmd << ' ' << shell_quote(normalize_flag_path(w));
            // -include takes a separate argument
            if (w == "-include") {
                skip_next = true;
            }
            continue;
        }

        // Collect source files (use paths from original command, not cmd.inputs)
        if (is_source_file(w)) {
            source_files.push_back(w);
        }
    }

    // Add source files extracted from original command
    for (auto const& src : source_files) {
        dep_cmd << ' ' << shell_quote(src);
    }

    return dep_cmd.str();
}

} // namespace

auto RulePatternRegistry::register_pattern(RulePattern pattern) -> void
{
    patterns_.push_back(std::move(pattern));
}

auto RulePatternRegistry::match_and_generate(CommandInfo const& cmd) const
    -> std::vector<GeneratedRule>
{
    auto result = std::vector<GeneratedRule> {};

    for (auto const& pattern : patterns_) {
        if (!std::regex_search(cmd.command, pattern.command_pattern)) {
            continue;
        }

        if (auto rule = pattern.generate(cmd)) {
            result.push_back(std::move(*rule));
        }
    }

    return result;
}

auto make_gcc_depfile_pattern() -> RulePattern
{
    return RulePattern {
        .command_pattern = std::regex { R"((gcc|g\+\+|clang|clang\+\+|cc|c\+\+).*\s-c\s)" },

        .generate = [](CommandInfo const& cmd) -> std::optional<GeneratedRule> {
            if (has_dep_flags(cmd.command)) {
                return std::nullopt;
            }

            auto display = cmd.inputs.empty()
                ? std::string { "DEP" }
                : "DEP " + cmd.inputs[0];

            auto dep_cmd = build_dep_scan_command(cmd);

            return GeneratedRule {
                .inputs = cmd.inputs,
                .order_only_inputs = cmd.order_only_inputs,
                .command = dep_cmd,
                .display = display,
                .outputs = { { .type = GeneratedOutput::Type::Stdout, .path = {} } },
                .action = OutputAction::InjectImplicitDeps,
                .parent_command = cmd.node_id,
            };
        },
    };
}

} // namespace pup::graph
