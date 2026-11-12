// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "ast.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <string_view>

namespace pup::parser {

/// Parse error with location and message
struct ParseError {
    SourceLocation location;
    StringId message = StringId::Empty;
};

/// Result of parsing a Tupfile
struct ParseResult {
    Tupfile tupfile;
    Vec<ParseError> errors;

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
    std::string_view filename
) -> ParseResult;

} // namespace pup::parser
