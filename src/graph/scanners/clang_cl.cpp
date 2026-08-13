// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/clang_cl.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
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

auto is_recognized_compile(std::span<std::string_view const> invocation) -> bool
{
    auto idx = driver_index(invocation);
    if (!idx) {
        return false;
    }
    return std::ranges::any_of(invocation.subspan(*idx + 1), is_compile_flag);
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

} // namespace

auto matches_clang_cl_compile(std::string_view command) -> bool
{
    auto tokens = tokenize_command(global_pool().intern(command));
    return !compile_prefix(tokens.invocations()).empty();
}

auto ClangClScanner::matches(CommandInfo const& /*cmd*/, CommandTokens const& tokens) const -> bool
{
    return !compile_prefix(tokens.invocations()).empty();
}

auto ClangClScanner::has_dep_flags(CommandTokens const& tokens) const -> bool
{
    // Only a pinned -MF counts: /MD and /MT select the CRT here, and a bare
    // -MD writes the depfile to the cwd instead of beside the object.
    return std::ranges::any_of(tokens.words(), [](auto word) {
        return word.starts_with("/clang:-MF") || word.starts_with("-clang:-MF");
    });
}

auto ClangClScanner::build_dep_scans(CommandInfo const& /*cmd*/, CommandTokens const& tokens) const
    -> Vec<DepScan>
{
    auto& pool = global_pool();
    auto scans = Vec<DepScan> {};

    for (auto invocation : compile_prefix(tokens.invocations())) {
        auto driver_idx = driver_index(invocation);
        if (!driver_idx) {
            continue;
        }

        auto dep_cmd = Buf {};
        for (auto i = std::size_t { 0 }; i <= *driver_idx; ++i) {
            if (i > 0) {
                dep_cmd += ' ';
            }
            dep_cmd += invocation[i];
        }

        dep_cmd += " /clang:-M";

        auto pending = std::optional<SeparateArg> {};
        auto redirected = false;
        auto source_files = Vec<std::string_view> {};
        auto object = std::string_view {};
        for (auto i = *driver_idx + 1; i < invocation.size(); ++i) {
            auto w = invocation[i];

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
                if (i < invocation.size()) {
                    object = invocation[i];
                }
                continue;
            }

            if (w.starts_with("/Fo") || w.starts_with("-Fo")) {
                object = w.substr(3);
                continue;
            }

            if (w.starts_with("-o") && w.size() > 2) {
                object = w.substr(2);
                continue;
            }

            // /link hands everything after it to the linker, source words included.
            if (w == "/link") {
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
