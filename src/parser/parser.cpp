// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/parser.hpp"
#include "pup/core/result.hpp"
#include "pup/parser/lexer.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <optional>

namespace pup::parser {

namespace {

struct RuleBody {
    Expression command;
    std::optional<Expression> display;
    std::vector<PathPattern> outputs;
    std::vector<PathPattern> extra_outputs;
    std::optional<std::string> output_group;
    std::optional<std::string> output_order_only_group;
    std::optional<Expression> output_order_only_group_dir;
};

struct ParserState {
    Lexer lexer;
    ParserOptions options;
    std::vector<ParseError> errors;
    std::vector<std::string> included_files;
    int include_depth = 0;

    Token current;
    Token previous;

    ParserState(std::string_view source, std::string_view filename)
        : lexer(source, filename)
    {
    }
};

// Forward declarations
auto advance(ParserState& s) -> Token;
auto check(ParserState const& s, TokenType type) -> bool;
auto match(ParserState& s, TokenType type) -> bool;
auto expect(ParserState& s, TokenType type, std::string_view message) -> Result<Token>;
auto skip_to_next_statement(ParserState& s) -> void;
auto parse_line(ParserState& s) -> Result<std::unique_ptr<Statement>>;
auto parse_rule(ParserState& s) -> Result<Rule>;
auto parse_bang_macro(ParserState& s) -> Result<BangMacro>;
auto parse_rule_body(ParserState& s) -> Result<RuleBody>;
auto parse_assignment(ParserState& s, Expression name_expr) -> Result<Assignment>;
auto parse_conditional(ParserState& s, Conditional::Kind kind) -> Result<Conditional>;
auto parse_include(ParserState& s, bool is_rules) -> Result<Include>;
auto parse_export(ParserState& s) -> Result<Export>;
auto parse_import(ParserState& s) -> Result<Import>;
auto parse_expression(ParserState& s) -> Result<Expression>;
auto parse_expression_until(ParserState& s, std::function<bool(Token const&)> const& stop, bool stop_at_gap = false) -> Result<Expression>;
auto parse_path_pattern(ParserState& s, bool stop_at_angle = false) -> Result<PathPattern>;
auto parse_command(ParserState& s) -> Result<Expression>;
auto report_error(ParserState& s, std::string const& message) -> void;

auto advance(ParserState& s) -> Token
{
    s.previous = s.current;
    s.current = s.lexer.next();
    return s.previous;
}

auto check(ParserState const& s, TokenType type) -> bool
{
    return s.current.type == type;
}

auto match(ParserState& s, TokenType type) -> bool
{
    if (check(s, type)) {
        advance(s);
        return true;
    }
    return false;
}

auto expect(ParserState& s, TokenType type, std::string_view message) -> Result<Token>
{
    if (check(s, type)) {
        return advance(s);
    }
    return pup::make_error<Token>(ErrorCode::UnexpectedToken, std::string { message } + ", got " + std::string { token_type_name(s.current.type) });
}

auto skip_to_next_statement(ParserState& s) -> void
{
    while (!check(s, TokenType::Eof) && !check(s, TokenType::Newline)) {
        advance(s);
    }
    if (check(s, TokenType::Newline)) {
        advance(s);
    }
}

auto report_error(ParserState& s, std::string const& message) -> void
{
    s.errors.push_back(ParseError {
        .location = s.current.location,
        .message = message,
    });
}

template<typename T>
auto make_statement(SourceLocation loc, Result<T> result) -> Result<std::unique_ptr<Statement>>
{
    if (!result) {
        return pup::unexpected<Error>(result.error());
    }

    auto stmt = std::make_unique<Statement>();
    stmt->location = loc;
    stmt->content = std::move(*result);
    return stmt;
}

auto skip_to_eol(ParserState& s) -> void
{
    while (!check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
        advance(s);
    }
    if (check(s, TokenType::Newline)) {
        advance(s);
    }
}

template<typename Predicate>
auto parse_statement_body(ParserState& s, Predicate is_terminator)
    -> std::vector<std::unique_ptr<Statement>>
{
    auto body = std::vector<std::unique_ptr<Statement>> {};

    while (!check(s, TokenType::Eof) && !is_terminator(s.current.type)) {
        if (match(s, TokenType::Newline) || match(s, TokenType::Hash)) {
            continue;
        }

        auto stmt = parse_line(s);
        if (!stmt) {
            report_error(s, stmt.error().message);
            skip_to_next_statement(s);
            continue;
        }
        if (*stmt) {
            body.push_back(std::move(*stmt));
        }
    }

    return body;
}

struct VarRefSpec {
    TokenType prefix_token;
    VarRef::Kind kind;
    char prefix_char;
    std::string_view unterminated_msg;
};

constexpr auto kVarRefSpecs = std::array {
    VarRefSpec { TokenType::Dollar, VarRef::Kind::Regular, '$', "Unterminated variable reference" },
    VarRefSpec { TokenType::At, VarRef::Kind::Config, '@', "Unterminated config variable reference" },
    VarRefSpec { TokenType::Ampersand, VarRef::Kind::Node, '&', "Unterminated node variable reference" },
};

auto try_parse_variable_ref(ParserState& s, VarRefSpec const& spec)
    -> std::optional<Result<VarRef>>
{
    if (!check(s, spec.prefix_token)) {
        return std::nullopt;
    }

    advance(s);
    if (!match(s, TokenType::OpenParen)) {
        return pup::make_error<VarRef>(
            ErrorCode::ParseError,
            std::format("Expected '(' after '{}'", spec.prefix_char)
        );
    }

    auto name = std::string {};
    while (!check(s, TokenType::CloseParen)
           && !check(s, TokenType::Newline)
           && !check(s, TokenType::Eof)) {
        name += s.current.text;
        advance(s);
    }

    if (!match(s, TokenType::CloseParen)) {
        return pup::make_error<VarRef>(
            ErrorCode::ParseError,
            std::string { spec.unterminated_msg }
        );
    }

    return VarRef { spec.kind, std::move(name), s.previous.location };
}

auto parse_line(ParserState& s) -> Result<std::unique_ptr<Statement>>
{
    auto const& tok = s.current;
    auto const start_loc = tok.location;

    // Rule: starts with ':'
    if (check(s, TokenType::Colon)) {
        advance(s);
        auto rule = parse_rule(s);
        if (!rule) {
            return pup::unexpected<Error>(rule.error());
        }

        auto stmt = std::make_unique<Statement>();
        stmt->location = start_loc;
        stmt->content = std::move(*rule);
        return stmt;
    }

    // Bang-macro: starts with '!'
    if (check(s, TokenType::Bang)) {
        advance(s);
        auto macro = parse_bang_macro(s);
        if (!macro) {
            return pup::unexpected<Error>(macro.error());
        }

        auto stmt = std::make_unique<Statement>();
        stmt->location = start_loc;
        stmt->content = std::move(*macro);
        return stmt;
    }

    // Keywords
    if (tok.is_keyword()) {
        switch (tok.type) {
        case TokenType::KwForeach:
            // foreach is only valid inside a rule
            return pup::make_error<std::unique_ptr<Statement>>(ErrorCode::ParseError, "'foreach' must appear after ':' in a rule");

        case TokenType::KwIncludeRules:
            advance(s);
            return make_statement(start_loc, parse_include(s, true));

        case TokenType::KwInclude:
            advance(s);
            return make_statement(start_loc, parse_include(s, false));

        case TokenType::KwIfdef:
            advance(s);
            return make_statement(start_loc, parse_conditional(s, Conditional::Kind::Ifdef));

        case TokenType::KwIfndef:
            advance(s);
            return make_statement(start_loc, parse_conditional(s, Conditional::Kind::Ifndef));

        case TokenType::KwIfeq:
            advance(s);
            return make_statement(start_loc, parse_conditional(s, Conditional::Kind::Ifeq));

        case TokenType::KwIfneq:
            advance(s);
            return make_statement(start_loc, parse_conditional(s, Conditional::Kind::Ifneq));

        case TokenType::KwElse:
        case TokenType::KwEndif:
            // These should be handled by parse_conditional
            return pup::make_error<std::unique_ptr<Statement>>(ErrorCode::ParseError, "Unexpected '" + std::string { tok.text } + "' without matching if");

        case TokenType::KwExport:
            advance(s);
            return make_statement(start_loc, parse_export(s));

        case TokenType::KwImport:
            advance(s);
            return make_statement(start_loc, parse_import(s));

        default:
            break;
        }
    }

    // Assignment: name (= | += | :=) value
    // name can be a complex expression like foo-$(BAR) or simple identifier
    if (check(s, TokenType::Identifier) || check(s, TokenType::Text) || check(s, TokenType::Dollar)) {
        // Parse the LHS as an expression, stopping at assignment operators
        auto name_expr = parse_expression_until(s, [](Token const& t) {
            return t.is_assignment_op() || t.is_end_of_statement();
        });
        if (!name_expr) {
            return pup::unexpected<Error>(name_expr.error());
        }

        if (s.current.is_assignment_op()) {
            auto assign = parse_assignment(s, std::move(*name_expr));
            if (!assign) {
                return pup::unexpected<Error>(assign.error());
            }
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*assign);
            return stmt;
        }

        // Not an assignment - skip rest of line (tup-like permissive behavior)
        while (!check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
            advance(s);
        }
        return nullptr;
    }

    // Node variable assignment: &name = value
    if (check(s, TokenType::Ampersand)) {
        advance(s);
        auto name_tok = expect(s, TokenType::Identifier, "Expected identifier after '&'");
        if (!name_tok) {
            return pup::unexpected<Error>(name_tok.error());
        }

        // Create a simple expression with just the identifier
        auto name_expr = Expression {};
        name_expr.parts.emplace_back(Expression::Literal { std::string { name_tok->text } });

        auto assign = parse_assignment(s, std::move(name_expr));
        if (!assign) {
            return pup::unexpected<Error>(assign.error());
        }

        assign->var_kind = VarRef::Kind::Node;
        auto stmt = std::make_unique<Statement>();
        stmt->location = start_loc;
        stmt->content = std::move(*assign);
        return stmt;
    }

    // Skip unrecognized lines (matching tup's permissive behavior)
    // This handles edge cases like 'echo "Root = " $(ROOT)' which tup treats
    // as a weird variable assignment. Rather than error, we skip to EOL.
    while (!check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
        advance(s);
    }

    return nullptr; // Return null to indicate skipped line
}

auto parse_rule(ParserState& s) -> Result<Rule>
{
    auto rule = Rule {};
    rule.location = s.previous.location;

    // Check for foreach
    if (match(s, TokenType::KwForeach)) {
        rule.foreach_ = true;
    }

    // Set context for input parsing
    s.lexer.set_context(Lexer::Context::Inputs);

    // Parse inputs until | or |>
    while (!check(s, TokenType::Pipe) && !check(s, TokenType::PipeArrow) && !check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
        auto pattern = parse_path_pattern(s);
        if (!pattern) {
            return pup::unexpected<Error>(pattern.error());
        }
        rule.inputs.push_back(std::move(*pattern));
    }

    // Parse order-only inputs if present
    if (match(s, TokenType::Pipe)) {
        while (!check(s, TokenType::PipeArrow) && !check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
            auto pattern = parse_path_pattern(s);
            if (!pattern) {
                return pup::unexpected<Error>(pattern.error());
            }
            rule.order_only_inputs.push_back(std::move(*pattern));
        }
    }

    // Parse rule body (command + outputs + groups)
    auto body = parse_rule_body(s);
    if (!body) {
        return pup::unexpected<Error>(body.error());
    }

    rule.command = std::move(body->command);
    rule.display = std::move(body->display);
    rule.outputs = std::move(body->outputs);
    rule.extra_outputs = std::move(body->extra_outputs);
    rule.output_group = std::move(body->output_group);
    rule.output_order_only_group = std::move(body->output_order_only_group);
    rule.output_order_only_group_dir = std::move(body->output_order_only_group_dir);

    s.lexer.set_context(Lexer::Context::LineStart);
    return rule;
}

auto parse_bang_macro(ParserState& s) -> Result<BangMacro>
{
    auto macro = BangMacro {};
    macro.location = s.previous.location;

    // Expect macro name
    if (!check(s, TokenType::Identifier) && !check(s, TokenType::Text)) {
        return pup::make_error<BangMacro>(ErrorCode::ParseError, "Expected macro name after '!'");
    }

    macro.name = std::string { s.current.text };
    advance(s);

    // Expect =
    auto eq = expect(s, TokenType::Equals, "Expected '=' after macro name");
    if (!eq) {
        return pup::unexpected<Error>(eq.error());
    }

    // Optional order-only inputs before |>
    if (match(s, TokenType::Pipe)) {
        while (!check(s, TokenType::PipeArrow) && !check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
            if (match(s, TokenType::KwForeach)) {
                macro.foreach_ = true;
                continue;
            }
            auto pattern = parse_path_pattern(s);
            if (!pattern) {
                return pup::unexpected<Error>(pattern.error());
            }
            macro.order_only_inputs.push_back(std::move(*pattern));
        }
    }

    // Parse rule body (command + outputs + groups)
    auto body = parse_rule_body(s);
    if (!body) {
        return pup::unexpected<Error>(body.error());
    }

    macro.command = std::move(body->command);
    macro.display = std::move(body->display);
    macro.outputs = std::move(body->outputs);
    macro.extra_outputs = std::move(body->extra_outputs);
    macro.output_group = std::move(body->output_group);
    macro.output_order_only_group = std::move(body->output_order_only_group);
    macro.output_order_only_group_dir = std::move(body->output_order_only_group_dir);

    s.lexer.set_context(Lexer::Context::LineStart);
    return macro;
}

auto parse_rule_body(ParserState& s) -> Result<RuleBody>
{
    auto body = RuleBody {};

    // Expect |> before command
    if (!check(s, TokenType::PipeArrow)) {
        return pup::make_error<RuleBody>(ErrorCode::ParseError, "Expected '|>' before command");
    }

    // Set context BEFORE advancing so next token is read in Command context
    s.lexer.set_context(Lexer::Context::Command);
    advance(s); // Consume |> and read command token in Command context

    // Parse command
    auto cmd = parse_command(s);
    if (!cmd) {
        return pup::unexpected<Error>(cmd.error());
    }
    body.command = std::move(*cmd);

    // Switch context before advancing so advance() reads Outputs context token
    s.lexer.set_context(Lexer::Context::Outputs);

    // Expect |> before outputs
    auto arrow2 = expect(s, TokenType::PipeArrow, "Expected '|>' after command");
    if (!arrow2) {
        return pup::unexpected<Error>(arrow2.error());
    }

    // Parse outputs (stop_at_angle=true so path/<group> splits the path from the group)
    while (!check(s, TokenType::Pipe) && !check(s, TokenType::OpenBrace) && !check(s, TokenType::OpenAngle) && !check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
        auto pattern = parse_path_pattern(s, true);
        if (!pattern) {
            return pup::unexpected<Error>(pattern.error());
        }
        body.outputs.push_back(std::move(*pattern));
    }

    // Parse extra outputs if present
    if (match(s, TokenType::Pipe)) {
        while (!check(s, TokenType::OpenBrace) && !check(s, TokenType::OpenAngle) && !check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
            auto pattern = parse_path_pattern(s, true);
            if (!pattern) {
                return pup::unexpected<Error>(pattern.error());
            }
            body.extra_outputs.push_back(std::move(*pattern));
        }
    }

    // Parse output group {name} if present
    if (match(s, TokenType::OpenBrace)) {
        if (check(s, TokenType::Identifier) || check(s, TokenType::Text)) {
            body.output_group = std::string { s.current.text };
            advance(s);
        }
        auto close = expect(s, TokenType::CloseBrace, "Expected '}' after group name");
        if (!close) {
            return pup::unexpected<Error>(close.error());
        }
    }

    // Parse order-only output group <name> if present
    // Supports path/<group> syntax where path specifies the group's directory
    if (match(s, TokenType::OpenAngle)) {
        // Check if last output is a directory prefix (ends with /)
        if (!body.outputs.empty()) {
            auto& last = body.outputs.back();
            if (!last.path.parts.empty()) {
                if (auto* lit = std::get_if<Expression::Literal>(&last.path.parts.back())) {
                    if (!lit->value.empty() && lit->value.back() == '/') {
                        body.output_order_only_group_dir = std::move(last.path);
                        body.outputs.pop_back();
                    }
                }
            }
        }

        if (check(s, TokenType::Identifier) || check(s, TokenType::Text)) {
            body.output_order_only_group = std::string { s.current.text };
            advance(s);
        }
        auto close = expect(s, TokenType::CloseAngle, "Expected '>' after group name");
        if (!close) {
            return pup::unexpected<Error>(close.error());
        }
    }

    return body;
}

auto parse_assignment(ParserState& s, Expression name_expr) -> Result<Assignment>
{
    auto assign = Assignment {};
    assign.location = s.previous.location;
    assign.name = std::move(name_expr);

    // Determine operation type
    if (match(s, TokenType::Equals)) {
        assign.op = Assignment::Op::Set;
    } else if (match(s, TokenType::PlusEquals)) {
        assign.op = Assignment::Op::Append;
    } else if (match(s, TokenType::ColonEquals)) {
        assign.op = Assignment::Op::Define;
    } else if (match(s, TokenType::QuestionEquals)) {
        assign.op = Assignment::Op::SoftSet;
    } else if (match(s, TokenType::DoubleQuestionEquals)) {
        assign.op = Assignment::Op::WeakSet;
    } else {
        return pup::make_error<Assignment>(ErrorCode::ParseError, "Expected assignment operator");
    }

    // Parse value (rest of line)
    auto value = parse_expression(s);
    if (!value) {
        return pup::unexpected<Error>(value.error());
    }
    assign.value = std::move(*value);

    return assign;
}

auto parse_conditional(ParserState& s, Conditional::Kind kind) -> Result<Conditional>
{
    auto cond = Conditional {};
    cond.location = s.previous.location;
    cond.kind = kind;

    if (kind == Conditional::Kind::Ifdef || kind == Conditional::Kind::Ifndef) {
        // ifdef/ifndef VAR
        if (!check(s, TokenType::Identifier) && !check(s, TokenType::Text)) {
            return pup::make_error<Conditional>(ErrorCode::ParseError, "Expected variable name after ifdef/ifndef");
        }
        cond.var_name = std::string { s.current.text };
        advance(s);
    } else {
        // ifeq/ifneq (lhs, rhs)
        auto open = expect(s, TokenType::OpenParen, "Expected '(' after ifeq/ifneq");
        if (!open) {
            return pup::unexpected<Error>(open.error());
        }

        s.lexer.set_context(Lexer::Context::Conditional);

        // Parse LHS until comma
        auto lhs = parse_expression_until(s, [](Token const& t) {
            return t.is(TokenType::Comma);
        });
        if (!lhs) {
            return pup::unexpected<Error>(lhs.error());
        }
        cond.lhs = std::move(*lhs);

        auto comma = expect(s, TokenType::Comma, "Expected ',' in ifeq/ifneq");
        if (!comma) {
            return pup::unexpected<Error>(comma.error());
        }

        // Parse RHS until close paren
        auto rhs = parse_expression_until(s, [](Token const& t) {
            return t.is(TokenType::CloseParen);
        });
        if (!rhs) {
            return pup::unexpected<Error>(rhs.error());
        }
        cond.rhs = std::move(*rhs);

        auto close = expect(s, TokenType::CloseParen, "Expected ')' in ifeq/ifneq");
        if (!close) {
            return pup::unexpected<Error>(close.error());
        }

        s.lexer.set_context(Lexer::Context::LineStart);
    }

    skip_to_eol(s);

    cond.then_body = parse_statement_body(s, [](TokenType t) {
        return t == TokenType::KwElse || t == TokenType::KwEndif;
    });

    if (match(s, TokenType::KwElse)) {
        skip_to_eol(s);
        cond.else_body = parse_statement_body(s, [](TokenType t) {
            return t == TokenType::KwEndif;
        });
    }

    // Expect endif
    if (!match(s, TokenType::KwEndif)) {
        return pup::make_error<Conditional>(ErrorCode::ParseError, "Expected 'endif'");
    }

    return cond;
}

auto parse_include(ParserState& s, bool is_rules) -> Result<Include>
{
    auto inc = Include {};
    inc.location = s.previous.location;
    inc.is_rules = is_rules;

    if (!is_rules) {
        auto path = parse_expression(s);
        if (!path) {
            return pup::unexpected<Error>(path.error());
        }
        inc.path = std::move(*path);
    }

    return inc;
}

auto parse_export(ParserState& s) -> Result<Export>
{
    auto exp = Export {};
    exp.location = s.previous.location;

    if (!check(s, TokenType::Identifier) && !check(s, TokenType::Text)) {
        return pup::make_error<Export>(ErrorCode::ParseError, "Expected variable name after 'export'");
    }

    exp.var_name = std::string { s.current.text };
    advance(s);

    return exp;
}

auto parse_import(ParserState& s) -> Result<Import>
{
    auto imp = Import {};
    imp.location = s.previous.location;

    if (!check(s, TokenType::Identifier) && !check(s, TokenType::Text)) {
        return pup::make_error<Import>(ErrorCode::ParseError, "Expected variable name after 'import'");
    }

    imp.var_name = std::string { s.current.text };
    advance(s);

    // Optional default value (=, ?=, and ??= are all equivalent for import)
    if (match(s, TokenType::Equals) || match(s, TokenType::QuestionEquals) || match(s, TokenType::DoubleQuestionEquals)) {
        auto value = parse_expression(s);
        if (!value) {
            return pup::unexpected<Error>(value.error());
        }
        imp.default_value = std::move(*value);
    }

    return imp;
}

auto parse_expression(ParserState& s) -> Result<Expression>
{
    return parse_expression_until(s, [](Token const& t) {
        return t.is_end_of_statement();
    });
}

auto parse_expression_until(
    ParserState& s,
    std::function<bool(Token const&)> const& stop,
    bool stop_at_gap
) -> Result<Expression>
{
    auto expr = Expression {};
    auto current_text = std::string {};
    auto last_end_offset = s.current.location.offset; // Track where last token ended

    auto flush_text = [&] {
        if (!current_text.empty()) {
            expr.parts.emplace_back(Expression::Literal { std::move(current_text) });
            current_text.clear();
        }
    };

    while (!check(s, TokenType::Eof) && !stop(s.current)) {
        // Check if there was whitespace between tokens (gap in offsets)
        auto has_gap = s.current.location.offset > last_end_offset;
        if (has_gap) {
            if (stop_at_gap) {
                flush_text();
                break; // Stop at whitespace gap (for path patterns)
            }
            // Add space to current_text for the gap
            // This handles both "text var" and "var text" cases
            current_text += ' ';
        }

        // Variable references: $(VAR), @(VAR), &(VAR)
        auto const* matched_spec = static_cast<VarRefSpec const*>(nullptr);
        for (auto const& spec : kVarRefSpecs) {
            if (check(s, spec.prefix_token)) {
                matched_spec = &spec;
                break;
            }
        }
        if (matched_spec) {
            flush_text();
            auto result = try_parse_variable_ref(s, *matched_spec);
            if (!result) {
                return pup::make_error<Expression>(ErrorCode::ParseError, "Internal parser error");
            }
            if (!*result) {
                return pup::unexpected<Error>(result->error());
            }

            expr.parts.emplace_back(Expression::Variable { std::move(**result) });
            last_end_offset = s.previous.location.offset + static_cast<std::uint32_t>(s.previous.text.size());
            continue;
        }

        // Any other token becomes text
        current_text += s.current.text;
        last_end_offset = s.current.location.offset + static_cast<std::uint32_t>(s.current.text.size());
        advance(s);
    }

    flush_text();
    return expr;
}

auto parse_path_pattern(ParserState& s, bool stop_at_angle) -> Result<PathPattern>
{
    auto pattern = PathPattern {};
    pattern.location = s.current.location;

    // Check for exclusion prefix (! for inputs, ^ for outputs)
    if (match(s, TokenType::Bang)) {
        pattern.is_exclusion = true;
    } else if (match(s, TokenType::Caret)) {
        pattern.is_output_exclusion = true;
    }

    // Check for group reference: {name} or <name>
    if (match(s, TokenType::OpenBrace)) {
        pattern.is_group = true;
        if (check(s, TokenType::Identifier) || check(s, TokenType::Text)) {
            pattern.group_name = std::string { s.current.text };
            advance(s);
        }
        if (!match(s, TokenType::CloseBrace)) {
            return pup::make_error<PathPattern>(ErrorCode::ParseError, "Expected '}' after group name");
        }
        return pattern;
    }

    if (match(s, TokenType::OpenAngle)) {
        pattern.is_order_only_group = true;
        if (check(s, TokenType::Identifier) || check(s, TokenType::Text)) {
            pattern.group_name = std::string { s.current.text };
            advance(s);
        }
        if (!match(s, TokenType::CloseAngle)) {
            return pup::make_error<PathPattern>(ErrorCode::ParseError, "Expected '>' after group name");
        }
        return pattern;
    }

    // Parse path expression (until whitespace or delimiter)
    // stop_at_gap=true ensures we stop at whitespace boundaries between paths
    // For outputs (stop_at_angle=true), also stop at < for path/<group> syntax
    auto path = parse_expression_until(s, [stop_at_angle](Token const& t) {
        if (stop_at_angle && t.is(TokenType::OpenAngle)) {
            return true;
        }
        return t.is_one_of(TokenType::Whitespace, TokenType::Pipe, TokenType::PipeArrow, TokenType::OpenBrace, TokenType::Newline, TokenType::Eof); }, true);
    if (!path) {
        return pup::unexpected<Error>(path.error());
    }
    pattern.path = std::move(*path);

    // Skip whitespace
    s.lexer.skip_whitespace();

    return pattern;
}

auto parse_command(ParserState& s) -> Result<Expression>
{
    auto expr = Expression {};
    auto tok = Token { s.current }; // Start with current token (already advanced past |>)

    // Collect command text until |>
    auto cmd_text = std::string {};
    while (!tok.is(TokenType::PipeArrow) && !tok.is(TokenType::Newline) && !tok.is(TokenType::Eof)) {
        cmd_text += tok.text;
        tok = Token { s.lexer.next() };
    }

    // Trim leading whitespace (O(n) using find_first_not_of + substr)
    if (auto pos = cmd_text.find_first_not_of(" \t"); pos != std::string::npos) {
        cmd_text = cmd_text.substr(pos);
    } else if (!cmd_text.empty() && (cmd_text.front() == ' ' || cmd_text.front() == '\t')) {
        cmd_text.clear();
    }

    // Handle display text: ^ text ^ at start of command
    // This handles the case where the entire command is one Text token
    if (!cmd_text.empty() && cmd_text[0] == '^') {
        // Find the second caret
        auto second_caret = std::size_t { cmd_text.find('^', 1) };
        if (second_caret != std::string::npos) {
            // Skip past the display text (including the second caret) and trim whitespace
            auto rest = cmd_text.substr(second_caret + 1);
            if (auto pos = rest.find_first_not_of(" \t"); pos != std::string::npos) {
                cmd_text = rest.substr(pos);
            } else {
                cmd_text.clear();
            }
        }
    }

    // Trim trailing whitespace
    while (!cmd_text.empty() && (cmd_text.back() == ' ' || cmd_text.back() == '\t')) {
        cmd_text.pop_back();
    }

    if (!cmd_text.empty()) {
        expr.parts.emplace_back(Expression::Literal { std::move(cmd_text) });
    }

    // Put back the |> token
    s.current = tok;

    return expr;
}

} // namespace

auto parse_tupfile(
    std::string_view source,
    std::string_view filename,
    ParserOptions opts
) -> ParseResult
{
    auto s = ParserState { source, filename };
    s.options = opts;
    advance(s); // Prime the parser with first token

    auto tupfile = Tupfile {};
    tupfile.filename = std::string { s.lexer.filename() };

    while (!check(s, TokenType::Eof)) {
        // Skip empty lines and comments
        if (match(s, TokenType::Newline)) {
            continue;
        }
        if (match(s, TokenType::Hash)) {
            if (!check(s, TokenType::Newline) && !check(s, TokenType::Eof)) {
                advance(s); // Skip to next line
            }
            continue;
        }

        auto stmt = parse_line(s);
        if (!stmt) {
            report_error(s, stmt.error().message);
            skip_to_next_statement(s);
            continue;
        }

        if (*stmt) {
            tupfile.statements.push_back(std::move(*stmt));
        }
    }

    return ParseResult {
        .tupfile = std::move(tupfile),
        .errors = std::move(s.errors),
    };
}

} // namespace pup::parser
