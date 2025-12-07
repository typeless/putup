// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include <cstdint>
#include <string_view>

namespace pup::parser {

/// Source location for error reporting
struct SourceLocation {
    std::string_view filename;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
};

/// Token types in Tupfile syntax
enum class TokenType : std::uint8_t {
    // Structural
    Eof,
    Newline,
    Whitespace,

    // Delimiters
    Colon,        // :
    Pipe,         // |
    PipeArrow,    // |>
    OpenParen,    // (
    CloseParen,   // )
    OpenBrace,    // {
    CloseBrace,   // }
    OpenBracket,  // [
    CloseBracket, // ]
    OpenAngle,    // <
    CloseAngle,   // >
    Comma,        // ,
    Equals,       // =
    ColonEquals,  // :=
    PlusEquals,   // +=
    Bang,         // !
    At,           // @
    Ampersand,    // &
    Dollar,       // $
    Percent,      // %
    Caret,        // ^
    Hash,         // #

    // Literals
    Identifier, // variable names, paths, filenames
    String,     // "quoted string" or 'quoted string'
    Text,       // unquoted text (commands, paths)

    // Keywords
    KwForeach,      // foreach
    KwInclude,      // include
    KwIncludeRules, // include_rules
    KwIfdef,        // ifdef
    KwIfndef,       // ifndef
    KwIfeq,         // ifeq
    KwIfneq,        // ifneq
    KwElse,         // else
    KwEndif,        // endif
    KwExport,       // export
    KwImport,       // import
    KwPreload,      // preload
    KwError,        // error
    KwRun,          // run
    KwGitignore,    // .gitignore

    // Error
    Invalid,
};

/// Convert token type to string for debugging
[[nodiscard]] constexpr auto token_type_name(TokenType type) -> std::string_view
{
    switch (type) {
    case TokenType::Eof:
        return "Eof";
    case TokenType::Newline:
        return "Newline";
    case TokenType::Whitespace:
        return "Whitespace";
    case TokenType::Colon:
        return "Colon";
    case TokenType::Pipe:
        return "Pipe";
    case TokenType::PipeArrow:
        return "PipeArrow";
    case TokenType::OpenParen:
        return "OpenParen";
    case TokenType::CloseParen:
        return "CloseParen";
    case TokenType::OpenBrace:
        return "OpenBrace";
    case TokenType::CloseBrace:
        return "CloseBrace";
    case TokenType::OpenBracket:
        return "OpenBracket";
    case TokenType::CloseBracket:
        return "CloseBracket";
    case TokenType::OpenAngle:
        return "OpenAngle";
    case TokenType::CloseAngle:
        return "CloseAngle";
    case TokenType::Comma:
        return "Comma";
    case TokenType::Equals:
        return "Equals";
    case TokenType::ColonEquals:
        return "ColonEquals";
    case TokenType::PlusEquals:
        return "PlusEquals";
    case TokenType::Bang:
        return "Bang";
    case TokenType::At:
        return "At";
    case TokenType::Ampersand:
        return "Ampersand";
    case TokenType::Dollar:
        return "Dollar";
    case TokenType::Percent:
        return "Percent";
    case TokenType::Caret:
        return "Caret";
    case TokenType::Hash:
        return "Hash";
    case TokenType::Identifier:
        return "Identifier";
    case TokenType::String:
        return "String";
    case TokenType::Text:
        return "Text";
    case TokenType::KwForeach:
        return "KwForeach";
    case TokenType::KwInclude:
        return "KwInclude";
    case TokenType::KwIncludeRules:
        return "KwIncludeRules";
    case TokenType::KwIfdef:
        return "KwIfdef";
    case TokenType::KwIfndef:
        return "KwIfndef";
    case TokenType::KwIfeq:
        return "KwIfeq";
    case TokenType::KwIfneq:
        return "KwIfneq";
    case TokenType::KwElse:
        return "KwElse";
    case TokenType::KwEndif:
        return "KwEndif";
    case TokenType::KwExport:
        return "KwExport";
    case TokenType::KwImport:
        return "KwImport";
    case TokenType::KwPreload:
        return "KwPreload";
    case TokenType::KwError:
        return "KwError";
    case TokenType::KwRun:
        return "KwRun";
    case TokenType::KwGitignore:
        return "KwGitignore";
    case TokenType::Invalid:
        return "Invalid";
    }
    return "Unknown";
}

/// A single token from the lexer
struct Token {
    TokenType type = TokenType::Invalid;
    std::string_view text;
    SourceLocation location;

    [[nodiscard]] auto is(TokenType t) const -> bool { return type == t; }

    [[nodiscard]] auto is_one_of(auto... types) const -> bool { return ((type == types) || ...); }

    [[nodiscard]] auto is_keyword() const -> bool
    {
        return type >= TokenType::KwForeach && type <= TokenType::KwGitignore;
    }

    [[nodiscard]] auto is_assignment_op() const -> bool
    {
        return is_one_of(TokenType::Equals, TokenType::ColonEquals, TokenType::PlusEquals);
    }

    [[nodiscard]] auto is_end_of_statement() const -> bool
    {
        return is_one_of(TokenType::Newline, TokenType::Eof);
    }
};

} // namespace pup::parser
