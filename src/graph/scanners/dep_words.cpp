// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/scanners/dep_words.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/vec.hpp"

#include <algorithm>
#include <cstddef>

namespace pup::graph::scanners {

namespace {

auto needs_shell_quoting(std::string_view s) -> bool
{
    return std::ranges::any_of(s, [](char c) {
        return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\\' || c == '$' || c == '`'
            || c == '!' || c == '*' || c == '?' || c == '[' || c == ']' || c == '(' || c == ')'
            || c == '{' || c == '}' || c == '<' || c == '>' || c == '|' || c == '&' || c == ';'
            || c == '#' || c == '~';
    });
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

auto shell_quote_into(Buf& out, std::string_view s) -> void
{
    if (!needs_shell_quoting(s)) {
        out += s;
        return;
    }

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

auto normalize_path_lexically_into(Buf& out, std::string_view path) -> void
{
    auto parts = Vec<std::string_view> {};
    auto start = std::size_t { 0 };
    auto is_absolute = !path.empty() && path[0] == '/';

    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        auto part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (part == ".." && !parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }
        start = end + 1;
    }

    if (parts.empty()) {
        out += is_absolute ? "/" : ".";
        return;
    }

    if (is_absolute) {
        out += '/';
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += '/';
        }
        out += parts[i];
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
