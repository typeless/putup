// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/lexer.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/token.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pup::parser {

Lexer::Lexer(std::string_view source, std::string_view filename)
    : filename_id_(global_pool().intern(filename))
{
    owned_source_.reserve(source.size());

    // A space, not tup's trim: the rewrite must stay length-preserving (token offsets, folded_newlines_); a trailing space is inert in every context — pinned by the trailing-whitespace property test.
    // Run class is [ \t\r], narrower than tup's isspace trim: a trailing \v/\f blocks the walk-back — #353.
    auto space_carriage_returns_ending_the_line = [this] {
        for (auto n = owned_source_.size(); n > 0; --n) {
            auto& c = owned_source_[n - 1];
            if (c == '\r') {
                c = ' ';
            } else if (c != ' ' && c != '\t') {
                break;
            }
        }
    };

    for (auto i = std::size_t { 0 }; i < source.size(); ++i) {
        auto const rest = source.substr(i);
        auto const continuation = rest.starts_with("\\\r\n") ? std::size_t { 3 }
            : rest.starts_with("\\\n")                       ? std::size_t { 2 }
            : rest == "\\\r"                                 ? std::size_t { 2 }
            : rest == "\\"                                   ? std::size_t { 1 }
                                                             : std::size_t { 0 };
        if (continuation == 0) {
            if (source[i] == '\n') {
                space_carriage_returns_ending_the_line();
            }
            owned_source_.push_back(source[i]);
            continue;
        }

        // Each byte becomes a space, as in tup (parser.c:589-596); the newline's own offset is
        // kept so diagnostics below a continuation still name their physical line, as tup's do.
        for (auto n = std::size_t { 0 }; n < continuation; ++n) {
            owned_source_.push_back(' ');
        }
        if (rest[continuation - 1] == '\n') {
            folded_newlines_.push_back(static_cast<std::uint32_t>(i + continuation - 1));
        }
        i += continuation - 1;
    }
    space_carriage_returns_ending_the_line();

    source_ = std::string_view { owned_source_.data(), owned_source_.size() };
}

auto Lexer::filename() const -> std::string_view
{
    return global_pool().get(filename_id_);
}

auto Lexer::next() -> Token
{
    if (peeked_) {
        auto token = *peeked_;
        peeked_.reset();
        return token;
    }
    return scan_token();
}

auto Lexer::peek() -> Token
{
    if (!peeked_) {
        peeked_ = scan_token();
    }
    return *peeked_;
}

auto Lexer::at_end() const -> bool
{
    return pos_ >= source_.size();
}

auto Lexer::location() const -> SourceLocation
{
    return SourceLocation {
        .filename = global_pool().get(filename_id_),
        .line = line_,
        .column = column_,
        .offset = static_cast<std::uint32_t>(pos_),
    };
}

auto Lexer::skip_to_eol() -> void
{
    while (!at_end() && peek_char() != '\n') {
        advance();
    }
}

auto Lexer::skip_whitespace() -> void
{
    while (!at_end()) {
        auto const c = peek_char();
        if (c == ' ' || c == '\t') {
            advance();
        } else {
            break;
        }
    }
}

auto Lexer::peek_char(std::size_t offset) const -> char
{
    auto const idx = pos_ + offset;
    return idx < source_.size() ? source_[idx] : '\0';
}

auto Lexer::match(char expected) -> bool
{
    if (peek_char() == expected) {
        advance();
        return true;
    }
    return false;
}

auto Lexer::match(std::string_view expected) -> bool
{
    if (source_.substr(pos_).starts_with(expected)) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            advance();
        }
        return true;
    }
    return false;
}

auto Lexer::advance() -> char
{
    if (at_end()) {
        return '\0';
    }
    auto const idx = pos_;
    auto const c = source_[pos_++];
    if (c == '\n') {
        advance_line();
    } else {
        ++column_;
    }

    // A folded newline ends a physical line without ending the logical one, so the line
    // advances and the context does not.
    if (next_folded_newline_ < folded_newlines_.size() && folded_newlines_[next_folded_newline_] == idx) {
        ++line_;
        column_ = 1;
        line_start_pos_ = pos_;
        ++next_folded_newline_;
    }
    return c;
}

auto Lexer::advance_line() -> void
{
    ++line_;
    column_ = 1;
    line_start_pos_ = pos_;
    context_ = Context::LineStart;
}

auto Lexer::putback() -> void
{
    if (pos_ > 0) {
        --pos_;
        --column_;
    }
}

auto Lexer::scan_token() -> Token
{
    // Skip whitespace (but not newlines) unless in command context
    if (context_ != Context::Command) {
        skip_whitespace();
    }

    if (at_end()) {
        return make_token(TokenType::Eof, pos_);
    }

    auto const start = pos_;
    auto const c = advance();

    // Newline
    if (c == '\n') {
        return make_token(TokenType::Newline, start);
    }

    // Comment
    if (c == '#') {
        skip_to_eol();
        return make_token(TokenType::Hash, start);
    }

    // In command context, check for |> first before scanning text
    if (context_ == Context::Command) {
        // Handle |> specially - it ends the command
        if (c == '|' && peek_char() == '>') {
            advance(); // consume '>'
            return make_token(TokenType::PipeArrow, start);
        }
        // Otherwise scan command text
        putback();
        return scan_command_text();
    }

    // Dot starts a path
    if (c == '.') {
        putback();
        return scan_text();
    }

    // Single-character tokens
    switch (c) {
    case '(':
        return make_token(TokenType::OpenParen, start);
    case ')':
        return make_token(TokenType::CloseParen, start);
    case '{':
        return make_token(TokenType::OpenBrace, start);
    case '}':
        return make_token(TokenType::CloseBrace, start);
    case '[':
        return make_token(TokenType::OpenBracket, start);
    case ']':
        return make_token(TokenType::CloseBracket, start);
    case '<':
        return make_token(TokenType::OpenAngle, start);
    case '>':
        return make_token(TokenType::CloseAngle, start);
    case ',':
        return make_token(TokenType::Comma, start);
    case '@':
        return make_token(TokenType::At, start);
    case '&':
        return make_token(TokenType::Ampersand, start);
    case '$':
        return make_token(TokenType::Dollar, start);
    case '^':
        return make_token(TokenType::Caret, start);
    case '!':
        return make_token(TokenType::Bang, start);
    default:
        break;
    }

    // % is special - could be standalone or part of pattern flag (%f, %B, etc.)
    if (c == '%') {
        // Check if followed by identifier character (makes it a pattern flag)
        if (!at_end() && is_identifier_char(peek_char())) {
            while (!at_end() && is_identifier_char(peek_char())) {
                advance();
            }
            // Also include trailing extension like %B.o
            if (peek_char() == '.') {
                advance();
                while (!at_end() && is_identifier_char(peek_char())) {
                    advance();
                }
            }
            return make_token(TokenType::Text, start);
        }
        return make_token(TokenType::Percent, start);
    }

    // Multi-character tokens
    if (c == ':') {
        if (match('=')) {
            return make_token(TokenType::ColonEquals, start);
        }
        return make_token(TokenType::Colon, start);
    }

    if (c == '+') {
        if (match('=')) {
            return make_token(TokenType::PlusEquals, start);
        }
        // + in text context is part of identifier/path
        putback();
        return scan_text();
    }

    if (c == '?') {
        if (match('?')) {
            if (match('=')) {
                return make_token(TokenType::DoubleQuestionEquals, start);
            }
            // ?? without = is not a valid token, treat as text
            putback();
            putback();
            return scan_text();
        }
        if (match('=')) {
            return make_token(TokenType::QuestionEquals, start);
        }
        // ? alone is part of text (glob pattern)
        putback();
        return scan_text();
    }

    if (c == '=') {
        return make_token(TokenType::Equals, start);
    }

    if (c == '|') {
        if (match('>')) {
            return make_token(TokenType::PipeArrow, start);
        }
        return make_token(TokenType::Pipe, start);
    }

    // Quoted strings
    if (c == '"' || c == '\'') {
        return scan_string(c);
    }

    // Identifier, keyword, or text
    if (is_identifier_start(c)) {
        putback();
        return scan_identifier_or_keyword();
    }

    // Text (paths, filenames, etc.)
    if (is_text_char(c)) {
        putback();
        return scan_text();
    }

    // Whitespace in command context
    if (c == ' ' || c == '\t') {
        skip_whitespace();
        return make_token(TokenType::Whitespace, start);
    }

    // Unknown character
    return make_token(TokenType::Invalid, start);
}

auto Lexer::scan_identifier_or_keyword() -> Token
{
    auto const start = pos_;

    // First, consume identifier characters
    while (!at_end() && is_identifier_char(peek_char())) {
        advance();
    }

    // Check if this continues as a path (contains /, ., *, ?)
    // If so, it's text not an identifier
    if (is_path_char(peek_char())) {
        // Continue scanning as text
        while (!at_end() && is_text_char(peek_char())) {
            advance();
        }
        return make_token(TokenType::Text, start);
    }

    auto const text = std::string_view { source_.substr(start, pos_ - start) };

    // Check for keywords
    if (auto kw = std::optional<TokenType> { keyword_type(text) }) {
        return make_token(*kw, start);
    }

    return make_token(TokenType::Identifier, start);
}

auto Lexer::scan_string(char quote) -> Token
{
    auto const start = pos_ - 1; // Include opening quote

    while (!at_end() && peek_char() != quote && peek_char() != '\n') {
        if (peek_char() == '\\' && peek_char(1) != '\0') {
            advance(); // Skip backslash
            advance(); // Skip escaped char
        } else {
            advance();
        }
    }

    if (peek_char() == quote) {
        advance(); // Consume closing quote
    }

    return make_token(TokenType::String, start);
}

auto Lexer::scan_text() -> Token
{
    auto const start = pos_;

    while (!at_end()) {
        auto const c = peek_char();

        // Stop at delimiters or variable references
        if (is_delimiter(c) || c == '$' || c == '@' || c == '&') {
            break;
        }

        advance();
    }

    return make_token(TokenType::Text, start);
}

auto Lexer::scan_command_text() -> Token
{
    auto const start = pos_;

    // Scan until |> or end of line
    while (!at_end()) {
        auto const c = peek_char();

        if (c == '\n') {
            break;
        }

        // Check for |>
        if (c == '|' && peek_char(1) == '>') {
            break;
        }

        advance();
    }

    // Trim trailing whitespace from command
    auto end = pos_;
    while (end > start && (source_[end - 1] == ' ' || source_[end - 1] == '\t')) {
        --end;
    }

    auto tok = Token { make_token(TokenType::Text, start) };
    tok.text = source_.substr(start, end - start);
    return tok;
}

auto Lexer::make_token(TokenType type, std::size_t start) const -> Token
{
    return Token {
        .type = type,
        .text = source_.substr(start, pos_ - start),
        .location = SourceLocation {
            .filename = global_pool().get(filename_id_),
            .line = line_,
            // A token may start on an earlier line than it ends on, and then it starts where
            // this line does.
            .column = static_cast<std::uint32_t>(start > line_start_pos_ ? start - line_start_pos_ + 1 : 1),
            .offset = static_cast<std::uint32_t>(start),
        },
    };
}

auto Lexer::is_identifier_start(char c) -> bool
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

auto Lexer::is_identifier_char(char c) -> bool
{
    return is_identifier_start(c) || (c >= '0' && c <= '9') || c == '-';
}

auto Lexer::is_text_char(char c) -> bool
{
    // Characters that can appear in paths, filenames, patterns
    return is_identifier_char(c) || c == '.' || c == '/' || c == '*' || c == '?' || c == '+' || c == '-' || c == '%';
}

auto Lexer::is_path_char(char c) -> bool
{
    // Characters that indicate this is a path rather than identifier
    return c == '/' || c == '.' || c == '*' || c == '?';
}

auto Lexer::is_delimiter(char c) -> bool
{
    constexpr auto delims = std::string_view { " \t\n#|:=(){}[]<>" };
    return delims.find(c) != std::string_view::npos;
}

auto Lexer::keyword_type(std::string_view text) -> std::optional<TokenType>
{
    if (text == "foreach") {
        return TokenType::KwForeach;
    }
    if (text == "include") {
        return TokenType::KwInclude;
    }
    if (text == "include_rules") {
        return TokenType::KwIncludeRules;
    }
    if (text == "ifdef") {
        return TokenType::KwIfdef;
    }
    if (text == "ifndef") {
        return TokenType::KwIfndef;
    }
    if (text == "ifeq") {
        return TokenType::KwIfeq;
    }
    if (text == "ifneq") {
        return TokenType::KwIfneq;
    }
    if (text == "else") {
        return TokenType::KwElse;
    }
    if (text == "endif") {
        return TokenType::KwEndif;
    }
    if (text == "export") {
        return TokenType::KwExport;
    }
    if (text == "import") {
        return TokenType::KwImport;
    }
    return std::nullopt;
}

} // namespace pup::parser
