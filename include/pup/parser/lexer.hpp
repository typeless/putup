// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "token.hpp"

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <optional>
#include <string_view>

namespace pup::parser {

/// Lexer for Tupfile syntax
///
/// The lexer is context-aware because Tupfile syntax is line-oriented:
/// - At line start: expects keywords, identifiers, ':', '!', or '#'
/// - After ':': expects inputs, '|', '|>'
/// - After '|>': expects command text (minimal tokenization)
/// - After second '|>': expects outputs, groups
///
/// The constructor rewrites every `\`-newline continuation to spaces (tup's
/// parse_tupfile), so no token text can span lines and no consumer of a
/// token — or of a command built from one — has to allow for an embedded newline.
class Lexer final {
public:
    /// Lexing context affects how text is tokenized
    enum class Context {
        LineStart,   ///< Beginning of line, expect directive/rule/assignment
        Inputs,      ///< After ':', before first '|>'
        Command,     ///< Between first and second '|>'
        Outputs,     ///< After second '|>'
        Conditional, ///< Inside ifeq/ifneq parentheses
    };

    explicit Lexer(std::string_view source, std::string_view filename = "<input>");

    // Tokens view the normalized source this owns, so a copy would hand out views into the original.
    Lexer(Lexer const&) = delete;
    auto operator=(Lexer const&) -> Lexer& = delete;

    /// Get next token (advances position)
    [[nodiscard]]
    auto next() -> Token;

    /// Peek at next token without advancing
    [[nodiscard]]
    auto peek() -> Token;

    /// Check if at end of input
    [[nodiscard]]
    auto at_end() const -> bool;

    /// Get current source location
    [[nodiscard]]
    auto location() const -> SourceLocation;

    /// Get current line number
    [[nodiscard]]
    auto line() const -> std::uint32_t
    {
        return line_;
    }

    /// Get current column number
    [[nodiscard]]
    auto column() const -> std::uint32_t
    {
        return column_;
    }

    /// Set lexing context
    auto set_context(Context ctx) -> void { context_ = ctx; }

    /// Get current lexing context
    [[nodiscard]]
    auto context() const -> Context
    {
        return context_;
    }

    /// Skip to end of current line (for error recovery or comments)
    auto skip_to_eol() -> void;

    /// Skip whitespace (not newlines)
    auto skip_whitespace() -> void;

    /// Get the full source text
    [[nodiscard]]
    auto source() const -> std::string_view
    {
        return source_;
    }

    /// Get filename
    [[nodiscard]]
    auto filename() const -> std::string_view;

private:
    Vec<char> owned_source_;
    Vec<std::uint32_t> folded_newlines_;
    std::size_t next_folded_newline_ = 0;
    std::string_view source_;
    StringId filename_id_ = StringId::Empty;
    std::size_t pos_ = 0;
    std::size_t line_start_pos_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t column_ = 1;
    Context context_ = Context::LineStart;
    std::optional<Token> peeked_;

    [[nodiscard]]
    auto peek_char(std::size_t offset = 0) const -> char;
    [[nodiscard]]
    auto match(char expected) -> bool;
    [[nodiscard]]
    auto match(std::string_view expected) -> bool;
    auto advance() -> char;
    auto advance_line() -> void;
    auto putback() -> void;

    [[nodiscard]]
    auto scan_token() -> Token;
    [[nodiscard]]
    auto scan_identifier_or_keyword() -> Token;
    [[nodiscard]]
    auto scan_string(char quote) -> Token;
    [[nodiscard]]
    auto scan_text() -> Token;
    [[nodiscard]]
    auto scan_command_text() -> Token;

    [[nodiscard]]
    auto make_token(TokenType type, std::size_t start) const -> Token;

    [[nodiscard]]
    static auto is_identifier_start(char c) -> bool;
    [[nodiscard]]
    static auto is_identifier_char(char c) -> bool;
    [[nodiscard]]
    static auto is_text_char(char c) -> bool;
    [[nodiscard]]
    static auto is_path_char(char c) -> bool;
    [[nodiscard]]
    static auto is_delimiter(char c) -> bool;
    [[nodiscard]]
    static auto keyword_type(std::string_view text) -> std::optional<TokenType>;
};

} // namespace pup::parser
