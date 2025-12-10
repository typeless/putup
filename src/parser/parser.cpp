// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/parser/parser.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace pup::parser {

// =============================================================================
// DefaultFileResolver
// =============================================================================

DefaultFileResolver::DefaultFileResolver(std::string_view root_dir)
    : root_dir_(root_dir)
{
}

auto DefaultFileResolver::resolve(std::string_view path, std::string_view relative_to)
    -> Result<std::string>
{
    namespace fs = std::filesystem;

    auto const rel_path = fs::path { fs::path { relative_to }.parent_path() / path };
    if (fs::exists(rel_path))
        return rel_path.string();

    auto const abs_path = fs::path { fs::path { root_dir_ } / path };
    if (fs::exists(abs_path))
        return abs_path.string();

    return pup::make_error<std::string>(ErrorCode::IncludeNotFound, "Include file not found: " + std::string { path });
}

auto DefaultFileResolver::read_file(std::string_view path) -> Result<std::string>
{
    auto file = std::ifstream { std::string { path } };
    if (!file)
        return pup::make_error<std::string>(ErrorCode::IoError, "Cannot open file: " + std::string { path });

    auto ss = std::stringstream {};
    ss << file.rdbuf();
    return ss.str();
}

auto DefaultFileResolver::find_tuprules(std::string_view from_dir) -> Result<std::string>
{
    namespace fs = std::filesystem;

    auto dir = fs::path { fs::path { from_dir } };
    auto const root = fs::path { fs::path { root_dir_ } };

    while (dir >= root) {
        auto const tuprules = fs::path { dir / "Tuprules.tup" };
        if (fs::exists(tuprules))
            return tuprules.string();
        if (dir == root)
            break;
        dir = dir.parent_path();
    }

    return pup::make_error<std::string>(ErrorCode::IncludeNotFound, "Tuprules.tup not found");
}

// =============================================================================
// Parser
// =============================================================================

Parser::Parser(std::string_view source, std::string_view filename,
    std::shared_ptr<FileResolver> resolver, Options options)
    : lexer_(source, filename)
    , resolver_(std::move(resolver))
    , options_(options)
{
    advance(); // Prime the parser with first token
}

auto Parser::parse() -> Result<Tupfile>
{
    auto tupfile = Tupfile {};
    tupfile.filename = std::string { lexer_.filename() };

    while (!check(TokenType::Eof)) {
        // Skip empty lines and comments
        if (match(TokenType::Newline))
            continue;
        if (match(TokenType::Hash)) {
            if (!check(TokenType::Newline) && !check(TokenType::Eof))
                advance(); // Skip to next line
            continue;
        }

        auto stmt = parse_line();
        if (!stmt) {
            report_error(stmt.error().message);
            skip_to_next_statement();
            continue;
        }

        if (*stmt) {
            // Handle gitignore flag
            if ((*stmt)->is<Gitignore>())
                tupfile.has_gitignore = true;

            tupfile.statements.push_back(std::move(*stmt));
        }
    }

    if (!errors_.empty()) {
        return pup::make_error<Tupfile>(ErrorCode::ParseError,
            "Parse failed with " + std::to_string(errors_.size()) + " error(s)");
    }

    return tupfile;
}

auto Parser::parse_statement() -> Result<std::unique_ptr<Statement>>
{
    while (match(TokenType::Newline) || match(TokenType::Hash))
        continue;

    if (check(TokenType::Eof))
        return nullptr;

    return parse_line();
}

auto Parser::advance() -> Token
{
    previous_ = current_;
    current_ = lexer_.next();
    return previous_;
}

auto Parser::check(TokenType type) const -> bool
{
    return current_.type == type;
}

auto Parser::match(TokenType type) -> bool
{
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

auto Parser::expect(TokenType type, std::string_view message) -> Result<Token>
{
    if (check(type))
        return advance();
    return pup::make_error<Token>(ErrorCode::UnexpectedToken,
        std::string { message } + ", got " + std::string { token_type_name(current_.type) });
}

auto Parser::skip_to_next_statement() -> void
{
    while (!check(TokenType::Eof) && !check(TokenType::Newline))
        advance();
    if (check(TokenType::Newline))
        advance();
}

auto Parser::parse_line() -> Result<std::unique_ptr<Statement>>
{
    auto const& tok = current_;
    auto const start_loc = tok.location;

    // Rule: starts with ':'
    if (check(TokenType::Colon)) {
        advance();
        auto rule = parse_rule();
        if (!rule)
            return pup::unexpected<Error>(rule.error());

        auto stmt = std::make_unique<Statement>();
        stmt->location = start_loc;
        stmt->content = std::move(*rule);
        return stmt;
    }

    // Bang-macro: starts with '!'
    if (check(TokenType::Bang)) {
        advance();
        auto macro = parse_bang_macro();
        if (!macro)
            return pup::unexpected<Error>(macro.error());

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
            return pup::make_error<std::unique_ptr<Statement>>(ErrorCode::ParseError,
                "'foreach' must appear after ':' in a rule");

        case TokenType::KwIncludeRules: {
            advance();
            auto inc = parse_include(true);
            if (!inc)
                return pup::unexpected<Error>(inc.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*inc);
            return stmt;
        }

        case TokenType::KwInclude: {
            advance();
            auto inc = parse_include(false);
            if (!inc)
                return pup::unexpected<Error>(inc.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*inc);
            return stmt;
        }

        case TokenType::KwIfdef: {
            advance();
            auto cond = parse_conditional(Conditional::Kind::Ifdef);
            if (!cond)
                return pup::unexpected<Error>(cond.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*cond);
            return stmt;
        }

        case TokenType::KwIfndef: {
            advance();
            auto cond = parse_conditional(Conditional::Kind::Ifndef);
            if (!cond)
                return pup::unexpected<Error>(cond.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*cond);
            return stmt;
        }

        case TokenType::KwIfeq: {
            advance();
            auto cond = parse_conditional(Conditional::Kind::Ifeq);
            if (!cond)
                return pup::unexpected<Error>(cond.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*cond);
            return stmt;
        }

        case TokenType::KwIfneq: {
            advance();
            auto cond = parse_conditional(Conditional::Kind::Ifneq);
            if (!cond)
                return pup::unexpected<Error>(cond.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*cond);
            return stmt;
        }

        case TokenType::KwElse:
        case TokenType::KwEndif:
            // These should be handled by parse_conditional
            return pup::make_error<std::unique_ptr<Statement>>(ErrorCode::ParseError,
                "Unexpected '" + std::string { tok.text } + "' without matching if");

        case TokenType::KwExport: {
            advance();
            auto exp = parse_export();
            if (!exp)
                return pup::unexpected<Error>(exp.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*exp);
            return stmt;
        }

        case TokenType::KwImport: {
            advance();
            auto imp = parse_import();
            if (!imp)
                return pup::unexpected<Error>(imp.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*imp);
            return stmt;
        }

        case TokenType::KwPreload: {
            advance();
            auto pre = parse_preload();
            if (!pre)
                return pup::unexpected<Error>(pre.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*pre);
            return stmt;
        }

        case TokenType::KwError: {
            advance();
            auto err = parse_error_directive();
            if (!err)
                return pup::unexpected<Error>(err.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*err);
            return stmt;
        }

        case TokenType::KwRun: {
            advance();
            auto run = parse_run();
            if (!run)
                return pup::unexpected<Error>(run.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*run);
            return stmt;
        }

        case TokenType::KwGitignore: {
            advance();
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = Gitignore {};
            return stmt;
        }

        default:
            break;
        }
    }

    // Assignment: name (= | += | :=) value
    // name can be a complex expression like foo-$(BAR) or simple identifier
    if (check(TokenType::Identifier) || check(TokenType::Text) || check(TokenType::Dollar)) {
        // Parse the LHS as an expression, stopping at assignment operators
        auto name_expr = parse_expression_until([](Token const& t) {
            return t.is_assignment_op() || t.is_end_of_statement();
        });
        if (!name_expr)
            return pup::unexpected<Error>(name_expr.error());

        if (current_.is_assignment_op()) {
            auto assign = parse_assignment(std::move(*name_expr));
            if (!assign)
                return pup::unexpected<Error>(assign.error());
            auto stmt = std::make_unique<Statement>();
            stmt->location = start_loc;
            stmt->content = std::move(*assign);
            return stmt;
        }

        // Not an assignment - skip rest of line (tup-like permissive behavior)
        while (!check(TokenType::Newline) && !check(TokenType::Eof))
            advance();
        return nullptr;
    }

    // Node variable assignment: &name = value
    if (check(TokenType::Ampersand)) {
        advance();
        auto name_tok = expect(TokenType::Identifier, "Expected identifier after '&'");
        if (!name_tok)
            return pup::unexpected<Error>(name_tok.error());

        // Create a simple expression with just the identifier
        auto name_expr = Expression {};
        name_expr.parts.emplace_back(Expression::Literal { std::string { name_tok->text } });

        auto assign = parse_assignment(std::move(name_expr));
        if (!assign)
            return pup::unexpected<Error>(assign.error());

        assign->var_kind = VarRef::Kind::Node;
        auto stmt = std::make_unique<Statement>();
        stmt->location = start_loc;
        stmt->content = std::move(*assign);
        return stmt;
    }

    // Skip unrecognized lines (matching tup's permissive behavior)
    // This handles edge cases like 'echo "Root = " $(ROOT)' which tup treats
    // as a weird variable assignment. Rather than error, we skip to EOL.
    while (!check(TokenType::Newline) && !check(TokenType::Eof))
        advance();

    return nullptr; // Return null to indicate skipped line
}

auto Parser::parse_rule() -> Result<Rule>
{
    auto rule = Rule {};
    rule.location = previous_.location;

    // Check for foreach
    if (match(TokenType::KwForeach))
        rule.foreach_ = true;

    // Set context for input parsing
    lexer_.set_context(Lexer::Context::Inputs);

    // Parse inputs until | or |>
    while (!check(TokenType::Pipe) && !check(TokenType::PipeArrow) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
        auto pattern = parse_path_pattern();
        if (!pattern)
            return pup::unexpected<Error>(pattern.error());
        rule.inputs.push_back(std::move(*pattern));
    }

    // Parse order-only inputs if present
    if (match(TokenType::Pipe)) {
        while (!check(TokenType::PipeArrow) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
            auto pattern = parse_path_pattern();
            if (!pattern)
                return pup::unexpected<Error>(pattern.error());
            rule.order_only_inputs.push_back(std::move(*pattern));
        }
    }

    // Expect |> before command
    if (!check(TokenType::PipeArrow))
        return pup::make_error<Rule>(ErrorCode::ParseError, "Expected '|>' before command");

    // Set context BEFORE advancing so next token is read in Command context
    lexer_.set_context(Lexer::Context::Command);
    advance(); // Consume |> and read command token in Command context

    // Parse command
    auto cmd = parse_command();
    if (!cmd)
        return pup::unexpected<Error>(cmd.error());
    rule.command = std::move(*cmd);

    // Switch context before advancing so advance() reads Outputs context token
    lexer_.set_context(Lexer::Context::Outputs);

    // Expect |> before outputs
    auto arrow2 = expect(TokenType::PipeArrow, "Expected '|>' after command");
    if (!arrow2)
        return pup::unexpected<Error>(arrow2.error());

    // Parse outputs (stop before {group} or <group> at end)
    while (!check(TokenType::Pipe) && !check(TokenType::OpenBrace) && !check(TokenType::OpenAngle) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
        auto pattern = parse_path_pattern();
        if (!pattern)
            return pup::unexpected<Error>(pattern.error());
        rule.outputs.push_back(std::move(*pattern));
    }

    // Parse extra outputs if present
    if (match(TokenType::Pipe)) {
        while (!check(TokenType::OpenBrace) && !check(TokenType::OpenAngle) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
            auto pattern = parse_path_pattern();
            if (!pattern)
                return pup::unexpected<Error>(pattern.error());
            rule.extra_outputs.push_back(std::move(*pattern));
        }
    }

    // Parse output group {name} if present
    if (match(TokenType::OpenBrace)) {
        if (check(TokenType::Identifier) || check(TokenType::Text)) {
            rule.output_group = std::string { current_.text };
            advance();
        }
        auto close = expect(TokenType::CloseBrace, "Expected '}' after group name");
        if (!close)
            return pup::unexpected<Error>(close.error());
    }

    // Parse order-only output group <name> if present
    if (match(TokenType::OpenAngle)) {
        if (check(TokenType::Identifier) || check(TokenType::Text)) {
            rule.output_order_only_group = std::string { current_.text };
            advance();
        }
        auto close = expect(TokenType::CloseAngle, "Expected '>' after group name");
        if (!close)
            return pup::unexpected<Error>(close.error());
    }

    lexer_.set_context(Lexer::Context::LineStart);
    return rule;
}

auto Parser::parse_bang_macro() -> Result<BangMacro>
{
    auto macro = BangMacro {};
    macro.location = previous_.location;

    // Expect macro name
    if (!check(TokenType::Identifier) && !check(TokenType::Text))
        return pup::make_error<BangMacro>(ErrorCode::ParseError, "Expected macro name after '!'");

    macro.name = std::string { current_.text };
    advance();

    // Expect =
    auto eq = expect(TokenType::Equals, "Expected '=' after macro name");
    if (!eq)
        return pup::unexpected<Error>(eq.error());

    // Optional order-only inputs before |>
    if (match(TokenType::Pipe)) {
        while (!check(TokenType::PipeArrow) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
            if (match(TokenType::KwForeach)) {
                macro.foreach_ = true;
                continue;
            }
            auto pattern = parse_path_pattern();
            if (!pattern)
                return pup::unexpected<Error>(pattern.error());
            macro.order_only_inputs.push_back(std::move(*pattern));
        }
    }

    // Expect |> before command
    if (!check(TokenType::PipeArrow))
        return pup::make_error<BangMacro>(ErrorCode::ParseError, "Expected '|>' before command");

    // Set context BEFORE advancing so next token is read in Command context
    lexer_.set_context(Lexer::Context::Command);
    advance(); // Consume |> and read command token in Command context

    // Parse command
    auto cmd = parse_command();
    if (!cmd)
        return pup::unexpected<Error>(cmd.error());
    macro.command = std::move(*cmd);

    // Switch context before advancing so advance() reads Outputs context token
    lexer_.set_context(Lexer::Context::Outputs);

    // Expect |> before outputs
    auto arrow2 = expect(TokenType::PipeArrow, "Expected '|>' after command");
    if (!arrow2)
        return pup::unexpected<Error>(arrow2.error());

    // Parse outputs
    while (!check(TokenType::Pipe) && !check(TokenType::OpenBrace) && !check(TokenType::OpenAngle) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
        auto pattern = parse_path_pattern();
        if (!pattern)
            return pup::unexpected<Error>(pattern.error());
        macro.outputs.push_back(std::move(*pattern));
    }

    // Parse extra outputs if present
    if (match(TokenType::Pipe)) {
        while (!check(TokenType::OpenBrace) && !check(TokenType::OpenAngle) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
            auto pattern = parse_path_pattern();
            if (!pattern)
                return pup::unexpected<Error>(pattern.error());
            macro.extra_outputs.push_back(std::move(*pattern));
        }
    }

    // Parse output group {name} if present
    if (match(TokenType::OpenBrace)) {
        if (check(TokenType::Identifier) || check(TokenType::Text)) {
            macro.output_group = std::string { current_.text };
            advance();
        }
        auto close = expect(TokenType::CloseBrace, "Expected '}' after group name");
        if (!close)
            return pup::unexpected<Error>(close.error());
    }

    // Parse order-only output group <name> if present
    if (match(TokenType::OpenAngle)) {
        if (check(TokenType::Identifier) || check(TokenType::Text)) {
            macro.output_order_only_group = std::string { current_.text };
            advance();
        }
        auto close = expect(TokenType::CloseAngle, "Expected '>' after group name");
        if (!close)
            return pup::unexpected<Error>(close.error());
    }

    lexer_.set_context(Lexer::Context::LineStart);
    return macro;
}

auto Parser::parse_assignment(Expression name_expr) -> Result<Assignment>
{
    auto assign = Assignment {};
    assign.location = previous_.location;
    assign.name = std::move(name_expr);

    // Determine operation type
    if (match(TokenType::Equals))
        assign.op = Assignment::Op::Set;
    else if (match(TokenType::PlusEquals))
        assign.op = Assignment::Op::Append;
    else if (match(TokenType::ColonEquals))
        assign.op = Assignment::Op::Define;
    else
        return pup::make_error<Assignment>(ErrorCode::ParseError, "Expected assignment operator");

    // Parse value (rest of line)
    auto value = parse_expression();
    if (!value)
        return pup::unexpected<Error>(value.error());
    assign.value = std::move(*value);

    return assign;
}

auto Parser::parse_conditional(Conditional::Kind kind) -> Result<Conditional>
{
    auto cond = Conditional {};
    cond.location = previous_.location;
    cond.kind = kind;

    if (kind == Conditional::Kind::Ifdef || kind == Conditional::Kind::Ifndef) {
        // ifdef/ifndef VAR
        if (!check(TokenType::Identifier) && !check(TokenType::Text))
            return pup::make_error<Conditional>(ErrorCode::ParseError,
                "Expected variable name after ifdef/ifndef");
        cond.var_name = std::string { current_.text };
        advance();
    } else {
        // ifeq/ifneq (lhs, rhs)
        auto open = expect(TokenType::OpenParen, "Expected '(' after ifeq/ifneq");
        if (!open)
            return pup::unexpected<Error>(open.error());

        lexer_.set_context(Lexer::Context::Conditional);

        // Parse LHS until comma
        auto lhs = parse_expression_until([](Token const& t) {
            return t.is(TokenType::Comma);
        });
        if (!lhs)
            return pup::unexpected<Error>(lhs.error());
        cond.lhs = std::move(*lhs);

        auto comma = expect(TokenType::Comma, "Expected ',' in ifeq/ifneq");
        if (!comma)
            return pup::unexpected<Error>(comma.error());

        // Parse RHS until close paren
        auto rhs = parse_expression_until([](Token const& t) {
            return t.is(TokenType::CloseParen);
        });
        if (!rhs)
            return pup::unexpected<Error>(rhs.error());
        cond.rhs = std::move(*rhs);

        auto close = expect(TokenType::CloseParen, "Expected ')' in ifeq/ifneq");
        if (!close)
            return pup::unexpected<Error>(close.error());

        lexer_.set_context(Lexer::Context::LineStart);
    }

    // Skip to end of line
    while (!check(TokenType::Newline) && !check(TokenType::Eof))
        advance();
    if (check(TokenType::Newline))
        advance();

    // Parse then body until else or endif
    while (!check(TokenType::KwElse) && !check(TokenType::KwEndif) && !check(TokenType::Eof)) {
        if (match(TokenType::Newline) || match(TokenType::Hash))
            continue;

        auto stmt = parse_line();
        if (!stmt) {
            report_error(stmt.error().message);
            skip_to_next_statement();
            continue;
        }
        if (*stmt)
            cond.then_body.push_back(std::move(*stmt));
    }

    // Parse else body if present
    if (match(TokenType::KwElse)) {
        while (!check(TokenType::Newline) && !check(TokenType::Eof))
            advance();
        if (check(TokenType::Newline))
            advance();

        while (!check(TokenType::KwEndif) && !check(TokenType::Eof)) {
            if (match(TokenType::Newline) || match(TokenType::Hash))
                continue;

            auto stmt = parse_line();
            if (!stmt) {
                report_error(stmt.error().message);
                skip_to_next_statement();
                continue;
            }
            if (*stmt)
                cond.else_body.push_back(std::move(*stmt));
        }
    }

    // Expect endif
    if (!match(TokenType::KwEndif))
        return pup::make_error<Conditional>(ErrorCode::ParseError, "Expected 'endif'");

    return cond;
}

auto Parser::parse_include(bool is_rules) -> Result<Include>
{
    auto inc = Include {};
    inc.location = previous_.location;
    inc.is_rules = is_rules;

    if (!is_rules) {
        auto path = parse_expression();
        if (!path)
            return pup::unexpected<Error>(path.error());
        inc.path = std::move(*path);
    }

    return inc;
}

auto Parser::parse_export() -> Result<Export>
{
    auto exp = Export {};
    exp.location = previous_.location;

    if (!check(TokenType::Identifier) && !check(TokenType::Text))
        return pup::make_error<Export>(ErrorCode::ParseError, "Expected variable name after 'export'");

    exp.var_name = std::string { current_.text };
    advance();

    return exp;
}

auto Parser::parse_import() -> Result<Import>
{
    auto imp = Import {};
    imp.location = previous_.location;

    if (!check(TokenType::Identifier) && !check(TokenType::Text))
        return pup::make_error<Import>(ErrorCode::ParseError, "Expected variable name after 'import'");

    imp.var_name = std::string { current_.text };
    advance();

    // Optional default value
    if (match(TokenType::Equals)) {
        auto value = parse_expression();
        if (!value)
            return pup::unexpected<Error>(value.error());
        imp.default_value = std::move(*value);
    }

    return imp;
}

auto Parser::parse_preload() -> Result<Preload>
{
    auto pre = Preload {};
    pre.location = previous_.location;

    auto path = parse_expression();
    if (!path)
        return pup::unexpected<Error>(path.error());
    pre.path = std::move(*path);

    return pre;
}

auto Parser::parse_error_directive() -> Result<ErrorDirective>
{
    auto err = ErrorDirective {};
    err.location = previous_.location;

    auto msg = parse_expression();
    if (!msg)
        return pup::unexpected<Error>(msg.error());
    err.message = std::move(*msg);

    return err;
}

auto Parser::parse_run() -> Result<Run>
{
    auto run = Run {};
    run.location = previous_.location;

    auto script = parse_expression();
    if (!script)
        return pup::unexpected<Error>(script.error());
    run.script = std::move(*script);

    return run;
}

auto Parser::parse_expression() -> Result<Expression>
{
    return parse_expression_until([](Token const& t) {
        return t.is_end_of_statement();
    });
}

auto Parser::parse_expression_until(std::function<bool(Token const&)> const& stop, bool stop_at_gap) -> Result<Expression>
{
    auto expr = Expression {};
    auto current_text = std::string {};
    auto last_end_offset = current_.location.offset; // Track where last token ended

    auto flush_text = [&] {
        if (!current_text.empty()) {
            expr.parts.emplace_back(Expression::Literal { std::move(current_text) });
            current_text.clear();
        }
    };

    while (!check(TokenType::Eof) && !stop(current_)) {
        // Check if there was whitespace between tokens (gap in offsets)
        auto has_gap = current_.location.offset > last_end_offset;
        if (has_gap) {
            if (stop_at_gap) {
                flush_text();
                break; // Stop at whitespace gap (for path patterns)
            }
            // Add space to current_text for the gap
            // This handles both "text var" and "var text" cases
            current_text += ' ';
        }

        // Variable reference: $(VAR)
        if (check(TokenType::Dollar)) {
            flush_text();
            advance();
            if (!match(TokenType::OpenParen))
                return pup::make_error<Expression>(ErrorCode::ParseError, "Expected '(' after '$'");

            auto name = std::string {};
            while (!check(TokenType::CloseParen) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
                name += current_.text;
                advance();
            }

            if (!match(TokenType::CloseParen))
                return pup::make_error<Expression>(ErrorCode::ParseError, "Unterminated variable reference");

            auto var = VarRef { VarRef::Kind::Regular, std::move(name), previous_.location };
            expr.parts.emplace_back(Expression::Variable { std::move(var) });
            last_end_offset = previous_.location.offset + static_cast<std::uint32_t>(previous_.text.size());
            continue;
        }

        // Config variable: @(VAR)
        if (check(TokenType::At)) {
            flush_text();
            advance();
            if (!match(TokenType::OpenParen))
                return pup::make_error<Expression>(ErrorCode::ParseError, "Expected '(' after '@'");

            auto name = std::string {};
            while (!check(TokenType::CloseParen) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
                name += current_.text;
                advance();
            }

            if (!match(TokenType::CloseParen))
                return pup::make_error<Expression>(ErrorCode::ParseError, "Unterminated config variable reference");

            auto var = VarRef { VarRef::Kind::Config, std::move(name), previous_.location };
            expr.parts.emplace_back(Expression::Variable { std::move(var) });
            last_end_offset = previous_.location.offset + static_cast<std::uint32_t>(previous_.text.size());
            continue;
        }

        // Node variable: &(VAR)
        if (check(TokenType::Ampersand)) {
            flush_text();
            advance();
            if (!match(TokenType::OpenParen))
                return pup::make_error<Expression>(ErrorCode::ParseError, "Expected '(' after '&'");

            auto name = std::string {};
            while (!check(TokenType::CloseParen) && !check(TokenType::Newline) && !check(TokenType::Eof)) {
                name += current_.text;
                advance();
            }

            if (!match(TokenType::CloseParen))
                return pup::make_error<Expression>(ErrorCode::ParseError, "Unterminated node variable reference");

            auto var = VarRef { VarRef::Kind::Node, std::move(name), previous_.location };
            expr.parts.emplace_back(Expression::Variable { std::move(var) });
            last_end_offset = previous_.location.offset + static_cast<std::uint32_t>(previous_.text.size());
            continue;
        }

        // Any other token becomes text
        current_text += current_.text;
        last_end_offset = current_.location.offset + static_cast<std::uint32_t>(current_.text.size());
        advance();
    }

    flush_text();
    return expr;
}

auto Parser::parse_path_pattern() -> Result<PathPattern>
{
    auto pattern = PathPattern {};
    pattern.location = current_.location;

    // Check for exclusion prefix (! for inputs, ^ for outputs)
    if (match(TokenType::Bang))
        pattern.is_exclusion = true;
    else if (match(TokenType::Caret))
        pattern.is_output_exclusion = true;

    // Check for group reference: {name} or <name>
    if (match(TokenType::OpenBrace)) {
        pattern.is_group = true;
        if (check(TokenType::Identifier) || check(TokenType::Text)) {
            pattern.group_name = std::string { current_.text };
            advance();
        }
        if (!match(TokenType::CloseBrace))
            return pup::make_error<PathPattern>(ErrorCode::ParseError, "Expected '}' after group name");
        return pattern;
    }

    if (match(TokenType::OpenAngle)) {
        pattern.is_order_only_group = true;
        if (check(TokenType::Identifier) || check(TokenType::Text)) {
            pattern.group_name = std::string { current_.text };
            advance();
        }
        if (!match(TokenType::CloseAngle))
            return pup::make_error<PathPattern>(ErrorCode::ParseError, "Expected '>' after group name");
        return pattern;
    }

    // Parse path expression (until whitespace or delimiter)
    // stop_at_gap=true ensures we stop at whitespace boundaries between paths
    auto path = parse_expression_until([](Token const& t) {
        return t.is_one_of(TokenType::Whitespace, TokenType::Pipe, TokenType::PipeArrow,
            TokenType::OpenBrace, TokenType::Newline, TokenType::Eof);
    },
        true);
    if (!path)
        return pup::unexpected<Error>(path.error());
    pattern.path = std::move(*path);

    // Skip whitespace
    lexer_.skip_whitespace();

    return pattern;
}

auto Parser::parse_command() -> Result<Expression>
{
    auto expr = Expression {};
    auto tok = Token { current_ }; // Start with current token (already advanced past |>)

    // Collect command text until |>
    auto cmd_text = std::string {};
    while (!tok.is(TokenType::PipeArrow) && !tok.is(TokenType::Newline) && !tok.is(TokenType::Eof)) {
        cmd_text += tok.text;
        tok = Token { lexer_.next() };
    }

    // Trim leading whitespace first
    while (!cmd_text.empty() && (cmd_text.front() == ' ' || cmd_text.front() == '\t'))
        cmd_text.erase(0, 1);

    // Handle display text: ^ text ^ at start of command
    // This handles the case where the entire command is one Text token
    if (!cmd_text.empty() && cmd_text[0] == '^') {
        // Find the second caret
        auto second_caret = std::size_t { cmd_text.find('^', 1) };
        if (second_caret != std::string::npos) {
            // Skip past the display text (including the second caret)
            cmd_text = cmd_text.substr(second_caret + 1);
            // Trim leading whitespace from actual command
            while (!cmd_text.empty() && (cmd_text.front() == ' ' || cmd_text.front() == '\t'))
                cmd_text.erase(0, 1);
        }
    }

    // Trim trailing whitespace
    while (!cmd_text.empty() && (cmd_text.back() == ' ' || cmd_text.back() == '\t'))
        cmd_text.pop_back();

    if (!cmd_text.empty())
        expr.parts.emplace_back(Expression::Literal { std::move(cmd_text) });

    // Put back the |> token
    current_ = tok;

    return expr;
}

auto Parser::make_error(std::string const& message) -> Error
{
    return Error::make(ErrorCode::ParseError,
        std::string { lexer_.filename() } + ":" + std::to_string(current_.location.line) + ":" + std::to_string(current_.location.column) + ": " + message);
}

auto Parser::report_error(std::string const& message) -> void
{
    errors_.push_back(ParseError {
        .location = current_.location,
        .message = message,
    });
}

} // namespace pup::parser
