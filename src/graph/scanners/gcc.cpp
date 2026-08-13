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

/// Where the compiler stands in one invocation -- first, or behind leading environment
/// assignments and one recognized wrapper. The words before it are the scan's to keep.
auto compiler_index(std::span<std::string_view const> invocation) -> std::optional<std::size_t>
{
    auto idx = std::size_t { 0 };
    while (idx < invocation.size() && is_env_assignment_word(invocation[idx])) {
        ++idx;
    }
    if (idx >= invocation.size()) {
        return std::nullopt;
    }
    if (is_compiler_wrapper(program_basename(invocation[idx])) && idx + 1 < invocation.size()) {
        ++idx;
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

/// The leading invocations a scan can draw from, ending at the last compile in them: an
/// invocation that is neither a compile nor scan-transparent may change the directory or the
/// environment, so it and everything after it are out of reach (#356, #352). Ending at the last
/// compile is what keeps a non-empty prefix and a non-empty scan set the same answer.
auto scannable_prefix(std::span<std::span<std::string_view const> const> invocations)
    -> std::span<std::span<std::string_view const> const>
{
    auto length = std::size_t { 0 };
    for (auto i = std::size_t { 0 }; i < invocations.size(); ++i) {
        if (is_recognized_compile(invocations[i])) {
            length = i + 1;
            continue;
        }
        if (!is_scan_transparent(invocations[i])) {
            break;
        }
    }
    return invocations.first(length);
}

} // namespace

auto matches_gcc_compile(std::string_view command) -> bool
{
    auto tokens = tokenize_command(global_pool().intern(command));
    return !scannable_prefix(tokens.invocations()).empty();
}

auto GccScanner::matches(CommandInfo const& /*cmd*/, CommandTokens const& tokens) const -> bool
{
    return !scannable_prefix(tokens.invocations()).empty();
}

auto GccScanner::has_dep_flags(CommandTokens const& tokens) const -> bool
{
    auto cmd = global_pool().get(tokens.text());
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

auto GccScanner::build_dep_scans(CommandInfo const& /*cmd*/, CommandTokens const& tokens) const
    -> Vec<DepScan>
{
    auto& pool = global_pool();
    auto scans = Vec<DepScan> {};

    for (auto invocation : scannable_prefix(tokens.invocations())) {
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
