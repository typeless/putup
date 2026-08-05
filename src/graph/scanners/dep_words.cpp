// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/dep_words.hpp"

#include "pup/core/buf.hpp"

#include <algorithm>
#include <cstddef>

namespace pup::graph::scanners {

namespace {

// The union of both shells' metacharacters: '^' is cmd.exe's escape character.
auto needs_shell_quoting(std::string_view s) -> bool
{
    return s.empty() || std::ranges::any_of(s, [](char c) {
               return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\\' || c == '$' || c == '`'
                   || c == '!' || c == '*' || c == '?' || c == '[' || c == ']' || c == '(' || c == ')'
                   || c == '{' || c == '}' || c == '<' || c == '>' || c == '|' || c == '&' || c == ';'
                   || c == '#' || c == '~' || c == '^';
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
