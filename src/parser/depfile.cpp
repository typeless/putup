// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/depfile.hpp"

#include <fstream>

namespace pup::parser {

namespace {

auto skip_whitespace(std::string_view& sv) -> void
{
    while (!sv.empty() && (sv[0] == ' ' || sv[0] == '\t')) {
        sv.remove_prefix(1);
    }
}

auto skip_line_continuation(std::string_view& sv) -> bool
{
    // Check for backslash followed by newline
    if (sv.size() >= 2 && sv[0] == '\\' && sv[1] == '\n') {
        sv.remove_prefix(2);
        return true;
    }
    // Handle \r\n line endings
    if (sv.size() >= 3 && sv[0] == '\\' && sv[1] == '\r' && sv[2] == '\n') {
        sv.remove_prefix(3);
        return true;
    }
    return false;
}

auto parse_path(std::string_view& sv, bool stop_at_colon = false) -> std::string
{
    auto result = std::string {};

    while (!sv.empty()) {
        auto c = sv[0];

        // End of path: whitespace or newline
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            break;
        }

        // Stop at colon when parsing target
        if (stop_at_colon && c == ':') {
            break;
        }

        // Check for line continuation
        if (c == '\\') {
            if (sv.size() >= 2) {
                auto next = sv[1];
                // Line continuation
                if (next == '\n') {
                    sv.remove_prefix(2);
                    skip_whitespace(sv);
                    continue;
                }
                if (next == '\r' && sv.size() >= 3 && sv[2] == '\n') {
                    sv.remove_prefix(3);
                    skip_whitespace(sv);
                    continue;
                }
                // Escaped space
                if (next == ' ') {
                    result += ' ';
                    sv.remove_prefix(2);
                    continue;
                }
                // Escaped backslash
                if (next == '\\') {
                    result += '\\';
                    sv.remove_prefix(2);
                    continue;
                }
                // Escaped hash (comment char)
                if (next == '#') {
                    result += '#';
                    sv.remove_prefix(2);
                    continue;
                }
            }
        }

        // Regular character
        result += c;
        sv.remove_prefix(1);
    }

    return result;
}

} // anonymous namespace

auto parse_depfile(std::string const& path) -> Result<Depfile>
{
    auto file = std::ifstream { path };
    if (!file) {
        return make_error<Depfile>(ErrorCode::IoError, "Failed to open depfile");
    }

    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    auto content = std::string(static_cast<std::size_t>(size), '\0');
    file.read(content.data(), size);
    return parse_depfile(std::string_view { content });
}

auto parse_depfile(std::string_view content) -> Result<Depfile>
{
    auto result = Depfile {};
    auto sv = content;

    // Skip leading whitespace
    skip_whitespace(sv);

    // Skip empty content
    if (sv.empty()) {
        return make_error<Depfile>(ErrorCode::ParseError, "Empty depfile");
    }

    // Parse target (output file), stopping at colon
    result.target = parse_path(sv, true);

    if (result.target.empty()) {
        return make_error<Depfile>(ErrorCode::ParseError, "Missing target in depfile");
    }

    // Skip whitespace after target
    skip_whitespace(sv);

    // Expect colon separator (might have been consumed with target)
    if (!sv.empty() && sv[0] == ':') {
        sv.remove_prefix(1);
    }

    // Parse dependencies
    while (!sv.empty()) {
        // Skip whitespace and line continuations
        skip_whitespace(sv);
        while (skip_line_continuation(sv)) {
            skip_whitespace(sv);
        }

        // Check for end of content or newline without continuation
        if (sv.empty()) {
            break;
        }
        if (sv[0] == '\n' || sv[0] == '\r') {
            break;
        }

        // Parse next dependency path
        auto dep = parse_path(sv);
        if (!dep.empty()) {
            result.dependencies.push_back(std::move(dep));
        }
    }

    return result;
}

} // namespace pup::parser
