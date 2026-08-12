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

// Words that would print something over the -M run's rule, or send it somewhere else.
constexpr std::string_view hazard_flags[] = {
    "-M",
    "/clang:-M",
    "-clang:-M",
    "/Fo",
    "-Fo",
    "/Fe",
    "/Fd",
    "/Fi",
    "/P",
    "/showIncludes",
};

auto separate_arg(std::string_view word) -> std::optional<SeparateArg>
{
    if (auto const* separate = find_separate_flag(separate_flags, word)) {
        return separate->kind;
    }
    return std::nullopt;
}

auto is_scan_hazard(std::string_view flag) -> bool
{
    return is_blank_word(flag) || has_shell_special(flag) || leads_any(hazard_flags, flag);
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

/// Where the driver stands in one invocation -- first, or behind one recognized wrapper.
auto driver_index(std::span<std::string_view const> invocation) -> std::optional<std::size_t>
{
    if (invocation.empty()) {
        return std::nullopt;
    }
    auto idx = std::size_t { 0 };
    if (is_compiler_wrapper(program_basename(invocation[0])) && invocation.size() > 1) {
        idx = 1;
    }
    if (!is_clang_cl_name(program_basename(invocation[idx]))) {
        return std::nullopt;
    }
    return idx;
}

/// A scan reproduces one invocation from the rule's directory, so it may carry a word only from a
/// command whose invocations are all compiles it recognizes (#356).
auto every_invocation_is_a_compile(std::span<std::span<std::string_view const> const> invocations) -> bool
{
    return std::ranges::all_of(invocations, [](auto invocation) {
        auto idx = driver_index(invocation);
        if (!idx) {
            return false;
        }
        return std::ranges::any_of(invocation.subspan(*idx + 1), is_compile_flag);
    });
}

auto command_words(std::string_view command) -> Vec<std::string_view>
{
    auto& pool = global_pool();
    auto words = Vec<std::string_view> {};
    for (auto id : core::tokenize_shell_command(command)) {
        words.push_back(pool.get(id));
    }
    return words;
}

} // namespace

auto matches_clang_cl_compile(std::string_view command) -> bool
{
    auto words = command_words(command);
    if (words.empty()) {
        return false;
    }

    auto invocations = split_invocations(std::span { words.data(), words.size() });
    return every_invocation_is_a_compile(std::span { invocations.data(), invocations.size() });
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
    auto words = command_words(pool.get(cmd.command));
    if (words.empty()) {
        return std::nullopt;
    }

    auto invocations = split_invocations(std::span { words.data(), words.size() });
    if (!every_invocation_is_a_compile(std::span { invocations.data(), invocations.size() })) {
        return std::nullopt;
    }

    auto const first = invocations[0];
    auto driver_idx = driver_index(first);
    if (!driver_idx) {
        return std::nullopt;
    }

    auto dep_cmd = Buf {};
    for (auto i = std::size_t { 0 }; i <= *driver_idx; ++i) {
        if (i > 0) {
            dep_cmd += ' ';
        }
        dep_cmd += first[i];
    }

    dep_cmd += " /clang:-M";

    auto pending = std::optional<SeparateArg> {};
    auto linker_tail = false;
    auto redirected = false;
    auto source_files = Vec<std::string_view> {};
    for (auto i = *driver_idx + 1; i < first.size(); ++i) {
        auto w = first[i];

        // A redirection hands its target to this same invocation, so the words after it are not
        // flags the scan may carry.
        if (is_flag_barrier(w)) {
            redirected = true;
            pending.reset();
            continue;
        }

        if (redirected) {
            if (is_source_file(w)) {
                source_files.push_back(w);
            }
            continue;
        }

        if (pending) {
            append_separate_arg_into(dep_cmd, w, *pending);
            pending.reset();
            continue;
        }

        if (is_compile_flag(w)) {
            continue;
        }

        if (w == "-o") {
            ++i;
            continue;
        }

        // /link hands everything after it to the linker, source words included -- and the rest of
        // the command with it, so no later invocation contributes one either.
        if (w == "/link") {
            linker_tail = true;
            break;
        }

        if (is_scan_hazard(w)) {
            continue;
        }

        if (is_source_file(w)) {
            source_files.push_back(w);
            continue;
        }

        dep_cmd += ' ';
        auto norm = Buf {};
        normalize_flag_path_into(norm, w);
        shell_quote_into(dep_cmd, norm.view());
        pending = separate_arg(w);
    }

    for (auto later = std::size_t { 1 }; !linker_tail && later < invocations.size(); ++later) {
        for (auto w : invocations[later]) {
            if (is_source_file(w)) {
                source_files.push_back(w);
            }
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
        .hazards = hazard_flags,
    };
}

auto make_clang_cl_scanner() -> std::unique_ptr<DepScanner>
{
    return std::make_unique<ClangClScanner>();
}

} // namespace pup::graph::scanners
