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

constexpr ArgFlag joined_flags[] = {
    { "-I", SeparateArg::Path },
    { "-isystem", SeparateArg::Path },
    { "-iquote", SeparateArg::Path },
    { "-include", SeparateArg::Path },
    { "-isysroot", SeparateArg::Path },
    { "--sysroot=", SeparateArg::Path },
    { "-D", SeparateArg::Value },
    { "-U", SeparateArg::Value },
};

constexpr ArgFlag separate_flags[] = {
    { "-I", SeparateArg::Path },
    { "-isystem", SeparateArg::Path },
    { "-iquote", SeparateArg::Path },
    { "-include", SeparateArg::Path },
    { "-isysroot", SeparateArg::Path },
    { "--sysroot", SeparateArg::Path },
    { "-D", SeparateArg::Value },
    { "-U", SeparateArg::Value },
};

// Words that would change what a -M run prints, or print a second depfile over it.
constexpr std::string_view hazard_flags[] = {
    "-M",
};

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

auto separate_arg(std::string_view flag) -> std::optional<SeparateArg>
{
    if (auto const* separate = find_separate_flag(separate_flags, flag)) {
        return separate->kind;
    }
    return std::nullopt;
}

auto is_scan_hazard(std::string_view flag) -> bool
{
    return is_blank_word(flag) || has_shell_special(flag) || leads_any(hazard_flags, flag);
}

/// Where the compiler stands in one invocation -- first, or behind one recognized wrapper.
auto compiler_index(std::span<std::string_view const> invocation) -> std::optional<std::size_t>
{
    if (invocation.empty()) {
        return std::nullopt;
    }
    auto idx = std::size_t { 0 };
    if (is_compiler_wrapper(program_basename(invocation[0])) && invocation.size() > 1) {
        idx = 1;
    }
    if (!is_compiler_name(program_basename(invocation[idx]))) {
        return std::nullopt;
    }
    return idx;
}

auto is_recognized_compile(std::span<std::string_view const> invocation) -> bool
{
    auto idx = compiler_index(invocation);
    if (!idx) {
        return false;
    }
    return std::ranges::any_of(invocation.subspan(*idx + 1), [](auto w) { return w == "-c"; });
}

/// The leading invocations a scan can reproduce from the rule's directory: one that is not a
/// compile may change the directory or the environment, so it and everything after it are out of
/// reach (#356).
auto compile_prefix(std::span<std::span<std::string_view const> const> invocations)
    -> std::span<std::span<std::string_view const> const>
{
    auto length = std::size_t { 0 };
    while (length < invocations.size() && is_recognized_compile(invocations[length])) {
        ++length;
    }
    return invocations.first(length);
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

auto matches_gcc_compile(std::string_view command) -> bool
{
    auto words = command_words(command);
    if (words.empty()) {
        return false;
    }

    auto invocations = split_invocations(std::span { words.data(), words.size() });
    return !compile_prefix(std::span { invocations.data(), invocations.size() }).empty();
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

auto GccScanner::build_dep_scans(CommandInfo const& cmd) const -> Vec<DepScan>
{
    auto& pool = global_pool();
    auto words = command_words(pool.get(cmd.command));
    if (words.empty()) {
        return {};
    }

    auto scans = Vec<DepScan> {};
    auto invocations = split_invocations(std::span { words.data(), words.size() });

    for (auto invocation : compile_prefix(std::span { invocations.data(), invocations.size() })) {
        auto compiler_idx = compiler_index(invocation);
        if (!compiler_idx) {
            continue;
        }

        auto dep_cmd = Buf {};

        for (auto i = std::size_t { 0 }; i <= *compiler_idx; ++i) {
            if (i > 0) {
                dep_cmd += ' ';
            }
            dep_cmd += invocation[i];
        }

        dep_cmd += " -M";

        auto pending = std::optional<SeparateArg> {};
        auto redirected = false;
        auto source_files = Vec<std::string_view> {};
        auto object = std::string_view {};
        for (auto i = *compiler_idx + 1; i < invocation.size(); ++i) {
            // A redirection hands its target to this same invocation, so the words after it are not
            // flags the scan may carry.
            if (is_flag_barrier(invocation[i])) {
                redirected = true;
                pending.reset();
                continue;
            }

            if (redirected) {
                if (is_source_file(invocation[i])) {
                    source_files.push_back(invocation[i]);
                }
                continue;
            }

            if (pending) {
                append_separate_arg_into(dep_cmd, invocation[i], *pending);
                pending.reset();
                continue;
            }

            auto w = invocation[i];

            if (w == "-c") {
                continue;
            }

            if (w == "-o") {
                ++i;
                if (i < invocation.size()) {
                    object = invocation[i];
                }
                continue;
            }

            if (w.starts_with("-o") && w.size() > 2) {
                object = w.substr(2);
                continue;
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

        if (source_files.empty()) {
            continue;
        }

        for (auto src : source_files) {
            dep_cmd += ' ';
            shell_quote_into(dep_cmd, src);
        }

        scans.push_back(DepScan {
            .command = pool.intern(dep_cmd.view()),
            .object = pool.intern(object),
        });
    }

    return scans;
}

auto GccScanner::dep_spec() const -> DepSpec
{
    return DepSpec {
        .output_mode = DepOutputMode::Stdout,
    };
}

auto gcc_flag_tables() -> FlagTables
{
    return FlagTables {
        .joined = joined_flags,
        .separate = separate_flags,
        .hazards = hazard_flags,
    };
}

auto make_gcc_scanner() -> std::unique_ptr<DepScanner>
{
    return std::make_unique<GccScanner>();
}

} // namespace pup::graph::scanners
