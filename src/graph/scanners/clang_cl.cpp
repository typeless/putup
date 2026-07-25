// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/clang_cl.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/scanners/dep_words.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>

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

/// Flags whose path argument is a separate word.
auto takes_separate_path(std::string_view word) -> bool
{
    static constexpr std::string_view separated[] = {
        "-I",
        "/I",
        "-D",
        "/D",
        "-U",
        "/U",
        "-isystem",
        "-iquote",
        "-include",
        "-isysroot",
        "--sysroot",
        "/imsvc",
        "/FI",
        "/external:I",
        "/winsysroot",
    };
    return std::ranges::find(separated, word) != std::ranges::end(separated);
}

auto is_dep_relevant_flag(std::string_view flag) -> bool
{
    if (has_shell_special(flag)) {
        return false;
    }

    static constexpr std::string_view prefixes[] = {
        "-I",
        "/I",
        "-D",
        "/D",
        "-U",
        "/U",
        "-isystem",
        "-iquote",
        "-include",
        "-isysroot",
        "--sysroot",
        "-std=",
        "/std:",
        "/imsvc",
        "/FI",
        "/external:I",
        "/winsysroot",
        "--target=",
        "/TP",
        "/TC",
        "/clang:",
    };
    return std::ranges::any_of(prefixes, [flag](auto p) { return flag.starts_with(p); });
}

auto normalize_flag_path_into(Buf& out, std::string_view flag) -> void
{
    for (auto const* prefix :
         { "-I", "/I", "-isystem", "-iquote", "-include", "-isysroot", "--sysroot=", "/imsvc", "/FI", "/external:I", "/winsysroot" }) {
        if (flag.starts_with(prefix)) {
            auto path = flag.substr(std::strlen(prefix));
            if (!path.empty()) {
                out += std::string_view { prefix };
                normalize_path_lexically_into(out, path);
                return;
            }
        }
    }
    out += flag;
}

auto append_quoted_path(Buf& dep_cmd, std::string_view word) -> void
{
    dep_cmd += ' ';
    auto norm = Buf {};
    normalize_path_lexically_into(norm, word);
    shell_quote_into(dep_cmd, norm.view());
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

    auto skip_next = false;
    auto source_files = Vec<std::string_view> {};
    for (auto i = driver_idx + 1; i < words.size(); ++i) {
        auto w = words[i];

        if (skip_next) {
            append_quoted_path(dep_cmd, w);
            skip_next = false;
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
            skip_next = takes_separate_path(w);
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

auto make_clang_cl_scanner() -> std::unique_ptr<DepScanner>
{
    return std::make_unique<ClangClScanner>();
}

} // namespace pup::graph::scanners
