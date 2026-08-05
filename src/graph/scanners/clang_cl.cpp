// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/clang_cl.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/scanners/dep_words.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <optional>

namespace pup::graph::scanners {

namespace {

auto is_clang_cl_name(std::string_view name) -> bool
{
    if (auto pos = name.rfind('-'); pos != std::string_view::npos) {
        auto suffix = name.substr(pos + 1);
        if (!suffix.empty()
            && std::ranges::all_of(suffix, [](char c) {
                   return std::isdigit(static_cast<unsigned char>(c)) || c == '.';
               })) {
            name = name.substr(0, pos);
        }
    }
    return name == "clang-cl" || name.ends_with("-clang-cl");
}

auto is_compile_flag(std::string_view word) -> bool
{
    return word == "-c" || word == "/c";
}

constexpr ArgFlag joined_flags[] = {
    { "-I", SeparateArg::Path },
    { "/I", SeparateArg::Path },
    { "-isystem", SeparateArg::Path },
    { "-iquote", SeparateArg::Path },
    { "-include", SeparateArg::Path },
    { "-isysroot", SeparateArg::Path },
    { "--sysroot=", SeparateArg::Path },
    { "/imsvc", SeparateArg::Path },
    { "/FI", SeparateArg::Path },
    { "/external:I", SeparateArg::Path },
    { "/winsysroot", SeparateArg::Path },
    { "-D", SeparateArg::Value },
    { "/D", SeparateArg::Value },
    { "-U", SeparateArg::Value },
    { "/U", SeparateArg::Value },
};

constexpr ArgFlag separate_flags[] = {
    { "-I", SeparateArg::Path },
    { "/I", SeparateArg::Path },
    { "-isystem", SeparateArg::Path },
    { "-iquote", SeparateArg::Path },
    { "-include", SeparateArg::Path },
    { "-isysroot", SeparateArg::Path },
    { "--sysroot", SeparateArg::Path },
    { "/imsvc", SeparateArg::Path },
    { "/FI", SeparateArg::Path },
    { "/external:I", SeparateArg::Path },
    { "/winsysroot", SeparateArg::Path },
    { "-D", SeparateArg::Value },
    { "/D", SeparateArg::Value },
    { "-U", SeparateArg::Value },
    { "/U", SeparateArg::Value },
};

constexpr std::string_view standalone_flags[] = {
    "-std=",
    "/std:",
    "--target=",
    "/TP",
    "/TC",
    "/clang:",
};

auto separate_arg(std::string_view word) -> std::optional<SeparateArg>
{
    if (auto const* separate = find_separate_flag(separate_flags, word)) {
        return separate->kind;
    }
    return std::nullopt;
}

auto is_dep_relevant_flag(std::string_view flag) -> bool
{
    if (has_shell_special(flag)) {
        return false;
    }

    return find_joined_flag(joined_flags, flag) != nullptr
        || find_separate_flag(separate_flags, flag) != nullptr
        || leads_any(standalone_flags, flag);
}

// Root detection is the host's: a cross-build's target-absolute path is treated as relative here.
auto normalize_flag_path_into(Buf& out, std::string_view flag) -> void
{
    if (auto const* joined = find_joined_flag(joined_flags, flag); joined && joined->kind == SeparateArg::Path) {
        auto path = flag.substr(joined->spelling.size());
        if (!path.empty()) {
            out += joined->spelling;
            out += global_pool().get(pup::path::normalize(path));
            return;
        }
    }
    out += flag;
}

} // namespace

auto matches_clang_cl_compile(std::string_view command) -> bool
{
    auto word_ids = core::tokenize_shell_command(command);
    if (word_ids.empty()) {
        return false;
    }

    auto& pool = global_pool();
    auto driver_idx = std::size_t { 0 };
    if (is_compiler_wrapper(program_basename(pool.get(word_ids[0]))) && word_ids.size() > 1) {
        driver_idx = 1;
    }

    if (!is_clang_cl_name(program_basename(pool.get(word_ids[driver_idx])))) {
        return false;
    }

    for (auto i = driver_idx + 1; i < word_ids.size(); ++i) {
        if (is_compile_flag(pool.get(word_ids[i]))) {
            return true;
        }
    }
    return false;
}

auto ClangClScanner::matches(CommandInfo const& cmd) const -> bool
{
    return matches_clang_cl_compile(global_pool().get(cmd.command));
}

auto ClangClScanner::has_dep_flags(std::string_view cmd) const -> bool
{
    // Only a pinned -MF counts: /MD and /MT select the CRT here, and a bare
    // -MD writes the depfile to the cwd instead of beside the object.
    auto& pool = global_pool();
    for (auto id : core::tokenize_shell_command(cmd)) {
        auto word = pool.get(id);
        if (word.starts_with("/clang:-MF") || word.starts_with("-clang:-MF")) {
            return true;
        }
    }
    return false;
}

auto ClangClScanner::build_dep_command(CommandInfo const& cmd) const -> std::optional<StringId>
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

    auto driver_idx = std::size_t { 0 };
    if (is_compiler_wrapper(program_basename(words[0])) && words.size() > 1) {
        driver_idx = 1;
    }

    if (!is_clang_cl_name(program_basename(words[driver_idx]))) {
        return std::nullopt;
    }

    auto dep_cmd = Buf {};
    for (auto i = std::size_t { 0 }; i <= driver_idx; ++i) {
        if (i > 0) {
            dep_cmd += ' ';
        }
        dep_cmd += words[i];
    }

    dep_cmd += " /clang:-M";

    auto pending = std::optional<SeparateArg> {};
    auto source_files = Vec<std::string_view> {};
    for (auto i = driver_idx + 1; i < words.size(); ++i) {
        auto w = words[i];

        if (pending) {
            append_separate_arg_into(dep_cmd, w, *pending);
            pending.reset();
            continue;
        }

        if (is_compile_flag(w) || w.starts_with("/Fo") || w.starts_with("-Fo")) {
            continue;
        }

        // A smuggled -MD would redirect the depfile and leave preprocessed
        // source on the stdout this scan parses.
        if (w.starts_with("/clang:-M") || w.starts_with("-clang:-M")) {
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

auto ClangClScanner::dep_spec() const -> DepSpec
{
    return DepSpec {
        .output_mode = DepOutputMode::Stdout,
    };
}

auto clang_cl_flag_tables() -> FlagTables
{
    return FlagTables {
        .joined = joined_flags,
        .separate = separate_flags,
        .standalone = standalone_flags,
    };
}

auto make_clang_cl_scanner() -> std::unique_ptr<DepScanner>
{
    return std::make_unique<ClangClScanner>();
}

} // namespace pup::graph::scanners
