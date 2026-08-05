// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/gcc.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/graph/scanners/dep_words.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>

namespace pup::graph::scanners {

namespace {

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

auto normalize_flag_path_into(Buf& out, std::string_view flag) -> void
{
    for (auto const* prefix : { "-I", "-isystem", "-iquote", "-include", "--sysroot=", "-isysroot" }) {
        if (flag.starts_with(prefix)) {
            auto path = flag.substr(std::strlen(prefix));
            if (!path.empty()) {
                out += std::string_view { prefix };
                out += global_pool().get(pup::path::normalize(path));
                return;
            }
        }
    }
    out += flag;
}

auto separate_arg(std::string_view flag) -> std::optional<SeparateArg>
{
    if (flag == "-D" || flag == "-U") {
        return SeparateArg::Value;
    }
    if (flag == "-I" || flag == "-include" || flag == "-isystem" || flag == "-iquote"
        || flag == "-isysroot") {
        return SeparateArg::Path;
    }
    return std::nullopt;
}

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

} // namespace

auto matches_gcc_compile(std::string_view command) -> bool
{
    auto word_ids = core::tokenize_shell_command(command);
    if (word_ids.empty()) {
        return false;
    }

    auto& pool = global_pool();
    auto compiler_idx = std::size_t { 0 };
    if (is_compiler_wrapper(program_basename(pool.get(word_ids[0]))) && word_ids.size() > 1) {
        compiler_idx = 1;
    }

    if (!is_compiler_name(program_basename(pool.get(word_ids[compiler_idx])))) {
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
        if (pos > 0 && !pup::core::is_space(cmd[pos - 1])) {
            ++pos;
            continue;
        }
        auto next_pos = pos + 2;
        if (next_pos >= cmd.size()) {
            return true;
        }
        auto c = cmd[next_pos];
        if (c == 'D' || c == 'M' || c == 'F' || c == 'G' || c == 'P' || c == 'T' || c == 'Q' || c == 'V'
            || pup::core::is_space(c)) {
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

    auto words = Vec<std::string_view> {};
    words.reserve(word_ids.size());
    for (auto id : word_ids) {
        words.push_back(pool.get(id));
    }

    auto compiler_idx = std::size_t { 0 };
    if (is_compiler_wrapper(program_basename(words[0])) && words.size() > 1) {
        compiler_idx = 1;
    }

    if (!is_compiler_name(program_basename(words[compiler_idx]))) {
        return std::nullopt;
    }

    auto dep_cmd = Buf {};

    for (auto i = std::size_t { 0 }; i <= compiler_idx; ++i) {
        if (i > 0) {
            dep_cmd += ' ';
        }
        dep_cmd += words[i];
    }

    dep_cmd += " -M";

    auto pending = std::optional<SeparateArg> {};
    auto source_files = Vec<std::string_view> {};
    for (auto i = compiler_idx + 1; i < words.size(); ++i) {
        if (pending) {
            append_separate_arg_into(dep_cmd, words[i], *pending);
            pending.reset();
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
            auto norm = Buf {};
            normalize_flag_path_into(norm, w);
            shell_quote_into(dep_cmd, norm.view());
            pending = separate_arg(w);
            continue;
        }

        if (is_source_file(w)) {
            source_files.push_back(w);
        }
    }

    if (source_files.empty()) {
        return std::nullopt;
    }

    for (auto src : source_files) {
        dep_cmd += ' ';
        shell_quote_into(dep_cmd, src);
    }

    return pool.intern(dep_cmd.view());
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
