// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/graph/scanners/dep_words.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pup::test {

/// The words a shell hands the program it launches, or nullopt where the shell
/// would have consumed a character instead of passing it through.
using ShellWords = std::optional<std::vector<std::string>>;

/// What `sh -c` does to a command line.
inline auto sh_split(std::string_view cmdline) -> ShellWords
{
    auto words = std::vector<std::string> {};
    auto current = std::string {};
    auto started = false;
    auto in_single = false;

    for (auto i = std::size_t { 0 }; i < cmdline.size(); ++i) {
        auto c = cmdline[i];
        if (in_single) {
            if (c == '\'') {
                in_single = false;
            } else {
                current += c;
            }
            continue;
        }
        if (c == '\\') {
            if (++i >= cmdline.size()) {
                return std::nullopt;
            }
            current += cmdline[i];
            started = true;
            continue;
        }
        if (c == '\'') {
            in_single = true;
            started = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (started) {
                words.push_back(current);
                current.clear();
                started = false;
            }
            continue;
        }
        if (std::string_view { "\"$`*?[]{}()<>|&;#~!" }.find(c) != std::string_view::npos) {
            return std::nullopt;
        }
        current += c;
        started = true;
    }
    if (in_single) {
        return std::nullopt;
    }
    if (started) {
        words.push_back(current);
    }
    return words;
}

/// What a program launched by `cmd.exe /c` sees, per the MSVC CRT rules: a
/// backslash run is halved only where it precedes a quote.
inline auto crt_split(std::string_view cmdline) -> ShellWords
{
    auto words = std::vector<std::string> {};
    auto current = std::string {};
    auto started = false;
    auto in_quotes = false;

    for (auto i = std::size_t { 0 }; i < cmdline.size();) {
        auto c = cmdline[i];
        if (c == '\\') {
            auto backslashes = std::size_t { 0 };
            while (i < cmdline.size() && cmdline[i] == '\\') {
                ++backslashes;
                ++i;
            }
            auto precedes_quote = i < cmdline.size() && cmdline[i] == '"';
            current.append(precedes_quote ? backslashes / 2 : backslashes, '\\');
            if (precedes_quote && backslashes % 2 == 1) {
                current += '"';
                ++i;
            }
            started = true;
            continue;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            started = true;
            ++i;
            continue;
        }
        if (!in_quotes && (c == ' ' || c == '\t')) {
            if (started) {
                words.push_back(current);
                current.clear();
                started = false;
            }
            ++i;
            continue;
        }
        if (!in_quotes && std::string_view { "^&|<>()%" }.find(c) != std::string_view::npos) {
            return std::nullopt;
        }
        current += c;
        started = true;
        ++i;
    }
    if (started) {
        words.push_back(current);
    }
    return words;
}

inline auto split_for(std::string_view cmdline, graph::scanners::QuoteStyle style) -> ShellWords
{
    return style == graph::scanners::QuoteStyle::Posix ? sh_split(cmdline) : crt_split(cmdline);
}

/// Splits the way the shell this build spawns commands through would.
inline auto split_for_host(std::string_view cmdline) -> ShellWords
{
    return split_for(cmdline, graph::scanners::host_quote_style);
}

} // namespace pup::test
