// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "ast.hpp"
#include "lexer.hpp"
#include "pup/core/result.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace pup::parser {

/// Parse error with location and message
struct ParseError {
    SourceLocation location;
    std::string message;
};

/// File resolver interface for include directives
class FileResolver {
public:
    virtual ~FileResolver() = default;
    FileResolver() = default;
    FileResolver(FileResolver const&) = delete;
    auto operator=(FileResolver const&) -> FileResolver& = delete;
    FileResolver(FileResolver&&) = delete;
    auto operator=(FileResolver&&) -> FileResolver& = delete;

    /// Resolve a path relative to the current file
    [[nodiscard]] virtual auto resolve(
        std::string_view path,
        std::string_view relative_to) -> Result<std::string> = 0;

    /// Read file contents
    [[nodiscard]] virtual auto read_file(std::string_view path) -> Result<std::string> = 0;

    /// Find Tuprules.tup by searching parent directories
    [[nodiscard]] virtual auto find_tuprules(std::string_view from_dir) -> Result<std::string> = 0;
};

/// Default file resolver using filesystem
class DefaultFileResolver : public FileResolver {
public:
    explicit DefaultFileResolver(std::string_view root_dir);

    [[nodiscard]] auto resolve(
        std::string_view path,
        std::string_view relative_to) -> Result<std::string> override;
    [[nodiscard]] auto read_file(std::string_view path) -> Result<std::string> override;
    [[nodiscard]] auto find_tuprules(std::string_view from_dir) -> Result<std::string> override;

private:
    std::string root_dir_;
};

/// Parser options
struct ParserOptions {
    bool process_includes = true;
    int max_include_depth = 100;
};

/// Parser for Tupfile syntax (PIMPL for compile-time isolation)
class Parser {
public:
    using Options = ParserOptions;

    Parser(std::string_view source, std::string_view filename,
        std::shared_ptr<FileResolver> resolver = nullptr, Options options = Options {});
    ~Parser();

    Parser(Parser const&) = delete;
    auto operator=(Parser const&) -> Parser& = delete;

    Parser(Parser&&) noexcept;
    auto operator=(Parser&&) noexcept -> Parser&;

    /// Parse complete Tupfile
    [[nodiscard]] auto parse() -> Result<Tupfile>;

    /// Parse single statement (for testing)
    [[nodiscard]] auto parse_statement() -> Result<std::unique_ptr<Statement>>;

    /// Get all parse errors
    [[nodiscard]] auto errors() const -> std::vector<ParseError> const&;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;

    // Token management
    auto advance() -> Token;
    [[nodiscard]] auto check(TokenType type) const -> bool;
    [[nodiscard]] auto match(TokenType type) -> bool;
    auto expect(TokenType type, std::string_view message) -> Result<Token>;
    auto skip_to_next_statement() -> void;

    // Statement parsing
    [[nodiscard]] auto parse_line() -> Result<std::unique_ptr<Statement>>;
    [[nodiscard]] auto parse_rule() -> Result<Rule>;
    [[nodiscard]] auto parse_bang_macro() -> Result<BangMacro>;
    struct RuleBody;
    [[nodiscard]] auto parse_rule_body() -> Result<RuleBody>;
    [[nodiscard]] auto parse_assignment(Expression name_expr) -> Result<Assignment>;
    [[nodiscard]] auto parse_conditional(Conditional::Kind kind) -> Result<Conditional>;
    [[nodiscard]] auto parse_include(bool is_rules) -> Result<Include>;
    [[nodiscard]] auto parse_export() -> Result<Export>;
    [[nodiscard]] auto parse_import() -> Result<Import>;
    [[nodiscard]] auto parse_preload() -> Result<Preload>;
    [[nodiscard]] auto parse_error_directive() -> Result<ErrorDirective>;
    [[nodiscard]] auto parse_run() -> Result<Run>;

    // Expression parsing
    [[nodiscard]] auto parse_expression() -> Result<Expression>;
    [[nodiscard]] auto parse_expression_until(
        std::function<bool(Token const&)> const& stop,
        bool stop_at_gap = false) -> Result<Expression>;
    [[nodiscard]] auto parse_path_pattern(bool stop_at_angle = false) -> Result<PathPattern>;
    [[nodiscard]] auto parse_path_list() -> Result<std::vector<PathPattern>>;
    [[nodiscard]] auto parse_path_list_until(TokenType stop) -> Result<std::vector<PathPattern>>;

    // Command parsing (between |> markers)
    [[nodiscard]] auto parse_command() -> Result<Expression>;

    // Helper to create error
    [[nodiscard]] auto make_error(std::string const& message) -> Error;
    auto report_error(std::string const& message) -> void;
};

} // namespace pup::parser
