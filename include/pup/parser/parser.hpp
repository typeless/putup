// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "ast.hpp"
#include "pup/core/string.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace pup::parser {

/// Parse error with location and message
struct ParseError {
    SourceLocation location;
    String message;
};

/// Parser options
struct ParserOptions {
    bool process_includes = true;
    int max_include_depth = 100;
};

/// Result of parsing a Tupfile
struct ParseResult {
    Tupfile tupfile;
    std::vector<ParseError> errors;

    [[nodiscard]]
    auto success() const -> bool
    {
        return errors.empty();
    }
};

/// Parse a Tupfile from source text
[[nodiscard]]
auto parse_tupfile(
    std::string_view source,
    std::string_view filename,
    ParserOptions opts = {}
) -> ParseResult;

} // namespace pup::parser
