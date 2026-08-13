// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/dep_words.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"

#include <algorithm>
#include <cstddef>

namespace pup::graph::scanners {

namespace {

// The union of both shells' metacharacters: '^' is cmd.exe's escape character.
auto needs_shell_quoting(std::string_view s) -> bool
{
    return s.empty() || std::ranges::any_of(s, [](char c) {
               return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '\\'
                   || c == '$' || c == '`' || c == '!' || c == '*' || c == '?' || c == '[' || c == ']'
                   || c == '(' || c == ')' || c == '{' || c == '}' || c == '<' || c == '>' || c == '|'
                   || c == '&' || c == ';' || c == '#' || c == '~' || c == '^';
           });
}

auto quote_posix_into(Buf& out, std::string_view s) -> void
{
    out += '\'';
    for (auto c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
}

// cmd.exe passes the tail on unsplit, so the quotes here are read by the child's
// MSVC CRT, which eats a backslash run only where it precedes a quote.
auto quote_windows_into(Buf& out, std::string_view s) -> void
{
    out += '"';
    for (auto i = std::size_t { 0 }; i < s.size();) {
        auto backslashes = std::size_t { 0 };
        while (i < s.size() && s[i] == '\\') {
            ++backslashes;
            ++i;
        }

        auto precedes_quote = i == s.size() || s[i] == '"';
        for (auto n = backslashes * (precedes_quote ? 2 : 1); n > 0; --n) {
            out += '\\';
        }

        if (i == s.size()) {
            break;
        }
        if (s[i] == '"') {
            out += "\\\"";
        } else {
            out += s[i];
        }
        ++i;
    }
    out += '"';
}

auto is_name_start(char c) -> bool
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

auto is_name_char(char c) -> bool
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

auto is_transparent_program(std::string_view name) -> bool
{
    return name == "echo" || name == "true" || name == ":";
}

// Words are split on whitespace, so one word can carry shell syntax that was never classified as
// such -- `FOO=1;` is an assignment the shell ends, `x>f.h` a redirection it never stood alone as.
auto hides_shell_syntax(std::string_view word) -> bool
{
    return has_shell_special(word) || word.find_first_of(";&|<>\n") != std::string_view::npos;
}

} // namespace

auto is_compiler_wrapper(std::string_view name) -> bool
{
    return name == "ccache" || name == "distcc" || name == "sccache" || name == "icecc";
}

auto has_shell_special(std::string_view flag) -> bool
{
    return flag.find('`') != std::string_view::npos || flag.find("$(") != std::string_view::npos;
}

auto shell_quote_into(Buf& out, std::string_view s, QuoteStyle style) -> void
{
    if (!needs_shell_quoting(s)) {
        out += s;
        return;
    }

    switch (style) {
    case QuoteStyle::Posix:
        quote_posix_into(out, s);
        return;
    case QuoteStyle::Windows:
        quote_windows_into(out, s);
        return;
    }
}

auto append_separate_arg_into(Buf& out, std::string_view word, SeparateArg kind) -> void
{
    out += ' ';
    switch (kind) {
    case SeparateArg::Path:
        shell_quote_into(out, global_pool().get(pup::path::normalize(word)));
        return;
    case SeparateArg::Value:
        shell_quote_into(out, word);
        return;
    }
}

auto is_blank_word(std::string_view word) -> bool
{
    // Emission net: quoting or an imported env value can spell a blank or newline word regardless of the lexer (#343, #347).
    return word.find_first_not_of(" \t\n\r") == std::string_view::npos;
}

auto is_flag_barrier(std::string_view word) -> bool
{
    if (is_invocation_separator(word)) {
        return true;
    }
    // A redirection may name its file descriptor first: `>log`, `1>log`, `2>&1`, `3<x`.
    auto rest = word.substr(std::min(word.find_first_not_of("0123456789"), word.size()));
    return rest.starts_with(">") || rest.starts_with("<");
}

auto is_invocation_separator(std::string_view word) -> bool
{
    return word == "&&" || word == "||" || word == ";" || word == "|" || word == "&";
}

auto is_env_assignment_word(std::string_view word) -> bool
{
    auto eq = word.find('=');
    if (eq == std::string_view::npos || eq == 0 || !is_name_start(word[0])) {
        return false;
    }
    return std::ranges::all_of(word.substr(0, eq), is_name_char) && !hides_shell_syntax(word);
}

auto is_scan_transparent(std::span<std::string_view const> invocation) -> bool
{
    if (invocation.empty()) {
        return true;
    }
    if (!is_transparent_program(program_basename(invocation[0]))) {
        return false;
    }
    return std::ranges::none_of(invocation, hides_shell_syntax);
}

auto split_invocations(std::span<std::string_view const> words) -> Vec<std::span<std::string_view const>>
{
    auto result = Vec<std::span<std::string_view const>> {};
    auto start = std::size_t { 0 };
    for (auto i = std::size_t { 0 }; i < words.size(); ++i) {
        if (is_invocation_separator(words[i])) {
            result.push_back(words.subspan(start, i - start));
            start = i + 1;
        }
    }
    // An operator with nothing after it ends the last invocation rather than beginning another; one
    // with nothing before it does begin an empty one, which each scanner classifies for itself.
    if (start < words.size() || result.empty()) {
        result.push_back(words.subspan(start));
    }
    return result;
}

auto find_joined_flag(std::span<ArgFlag const> table, std::string_view word) -> ArgFlag const*
{
    for (auto const& flag : table) {
        if (word.starts_with(flag.spelling)) {
            return &flag;
        }
    }
    return nullptr;
}

auto find_separate_flag(std::span<ArgFlag const> table, std::string_view word) -> ArgFlag const*
{
    for (auto const& flag : table) {
        if (word == flag.spelling) {
            return &flag;
        }
    }
    return nullptr;
}

auto leads_any(std::span<std::string_view const> spellings, std::string_view word) -> bool
{
    return std::ranges::any_of(spellings, [word](auto s) { return word.starts_with(s); });
}

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

auto program_basename(std::string_view word) -> std::string_view
{
    if (auto pos = word.rfind('/'); pos != std::string_view::npos) {
        word = word.substr(pos + 1);
    }
    if (word.ends_with(".exe")) {
        word = word.substr(0, word.size() - 4);
    }
    return word;
}

} // namespace pup::graph::scanners
