// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/parser.hpp"
#include "xfrontend/source_manager.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace xfrontend {
namespace {

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return value;
}

bool is_name_token(const Token& token) {
    return token.kind == TokenKind::Identifier || token.kind == TokenKind::Keyword;
}

bool is_prefix_operator(const Token& token) {
    if (token.kind != TokenKind::Operator) {
        return false;
    }
    return token.lexeme == "!" || token.lexeme == "~" || token.lexeme == "+" ||
           token.lexeme == "-" || token.lexeme == "*" || token.lexeme == "&";
}

bool begins_expression(const Token& token) {
    if (token.kind == TokenKind::Identifier || token.kind == TokenKind::IntegerLiteral ||
        token.kind == TokenKind::FloatingLiteral || token.kind == TokenKind::StringLiteral ||
        token.kind == TokenKind::LeftParen || token.kind == TokenKind::LeftBracket) {
        return true;
    }
    if (token.kind == TokenKind::Keyword) {
        const std::string folded = lower_ascii(token.lexeme);
        return folded == "true" || folded == "false" || folded == "null";
    }
    return is_prefix_operator(token);
}

} // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
    if (tokens_.empty() || tokens_.back().kind != TokenKind::EndOfFile) {
        Token eof;
        eof.kind = TokenKind::EndOfFile;
        if (!tokens_.empty()) {
            eof.span = tokens_.back().span;
        }
        tokens_.push_back(std::move(eof));
    }
}

ParseResult Parser::run() {
    parse_module_and_imports();
    parse_top_level_declarations();
    return std::move(result_);
}

const Token& Parser::peek(std::size_t lookahead) const noexcept {
    const std::size_t index = std::min(current_ + lookahead, tokens_.size() - 1);
    return tokens_[index];
}

const Token& Parser::previous() const noexcept {
    return tokens_[current_ == 0 ? 0 : current_ - 1];
}

bool Parser::at_end() const noexcept {
    return peek().kind == TokenKind::EndOfFile;
}

bool Parser::check(TokenKind kind) const noexcept {
    return peek().kind == kind;
}

bool Parser::check_keyword(const char* keyword) const {
    return peek().kind == TokenKind::Keyword && lower_ascii(peek().lexeme) == keyword;
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

bool Parser::match_keyword(const char* keyword) {
    if (!check_keyword(keyword)) {
        return false;
    }
    advance();
    return true;
}

const Token& Parser::advance() {
    if (!at_end()) {
        ++current_;
    }
    return previous();
}

bool Parser::expect(TokenKind kind, const char* message) {
    if (match(kind)) {
        return true;
    }
    add_error("XPARSE0001", message, peek().span);
    return false;
}

bool Parser::expect_identifier(const char* message) {
    if (peek().kind == TokenKind::Identifier) {
        advance();
        return true;
    }
    add_error("XPARSE0002", message, peek().span);
    return false;
}

void Parser::add_error(const char* code, const char* message, const SourceSpan& span) {
    result_.diagnostics.push_back(Diagnostic{code, message, span});
}

void Parser::synchronize_top_level() {
    while (!at_end()) {
        if (previous().kind == TokenKind::Semicolon) {
            return;
        }
        if (check_keyword("function") || check_keyword("public") ||
            check_keyword("private") || check_keyword("internal") ||
            check_keyword("module") || check_keyword("import")) {
            return;
        }
        advance();
    }
}

void Parser::synchronize_statement() {
    while (!at_end() && !check(TokenKind::RightBrace)) {
        if (previous().kind == TokenKind::Semicolon) {
            return;
        }
        if (check_keyword("if") || check_keyword("while") || check_keyword("for") ||
            check_keyword("return") || check_keyword("break") || check_keyword("continue")) {
            return;
        }
        advance();
    }
}

void Parser::parse_module_and_imports() {
    if (match_keyword("module")) {
        const Token start = previous();
        result_.unit.module_name = parse_qualified_name("Expected a module name after 'module'");
        if (!expect(TokenKind::Semicolon, "Expected ';' after module declaration")) {
            synchronize_top_level();
        }
        (void)start;
    }

    while (match_keyword("import")) {
        if (saw_function_) {
            add_error("XPARSE0003", "Imports must appear before function declarations", previous().span);
        }
        const std::string name = parse_qualified_name("Expected a module name after 'import'");
        if (!name.empty()) {
            result_.unit.imports.push_back(name);
            result_.unit.import_spans.push_back(previous().span);
        }
        if (!expect(TokenKind::Semicolon, "Expected ';' after import declaration")) {
            synchronize_top_level();
        }
    }
}

std::string Parser::parse_qualified_name(const char* context) {
    if (peek().kind != TokenKind::Identifier) {
        add_error("XPARSE0004", context, peek().span);
        return {};
    }
    std::string name = advance().lexeme;
    while (match(TokenKind::Dot)) {
        if (peek().kind != TokenKind::Identifier) {
            add_error("XPARSE0005", "Expected an identifier after '.' in qualified name", peek().span);
            break;
        }
        name += "." + advance().lexeme;
    }
    return name;
}

void Parser::parse_top_level_declarations() {
    while (!at_end()) {
        const std::size_t start = current_;
        if (check_keyword("module")) {
            add_error("XPARSE0006", "Module declaration must be the first declaration", peek().span);
            advance();
            synchronize_top_level();
        } else if (check_keyword("import")) {
            add_error("XPARSE0003", "Imports must appear before function declarations", peek().span);
            advance();
            synchronize_top_level();
        } else if (parse_function()) {
            saw_function_ = true;
        } else {
            add_error("XPARSE0007", "Expected a function declaration at module scope", peek().span);
            synchronize_top_level();
        }
        if (current_ == start && !at_end()) {
            advance();
        }
    }
}

bool Parser::parse_function() {
    const std::size_t start_index = current_;
    Visibility visibility = Visibility::Private;
    if (match_keyword("public")) {
        visibility = Visibility::Public;
    } else if (match_keyword("private")) {
        visibility = Visibility::Private;
    } else if (match_keyword("internal")) {
        visibility = Visibility::Internal;
    }

    if (!match_keyword("function")) {
        current_ = start_index;
        return false;
    }

    FunctionDeclaration function;
    function.visibility = visibility;
    const SourcePosition start = tokens_[start_index].span.start;
    if (peek().kind != TokenKind::Identifier) {
        add_error("XPARSE0008", "Expected function name", peek().span);
        synchronize_top_level();
        return true;
    }
    function.name = advance().lexeme;

    if (match(TokenKind::LeftBracket)) {
        while (!at_end() && !check(TokenKind::RightBracket)) {
            std::string generic;
            if (is_name_token(peek())) {
                generic = advance().lexeme;
                if (match(TokenKind::Colon)) {
                    if (!is_name_token(peek())) {
                        add_error("XPARSE0009", "Expected a generic constraint name", peek().span);
                    } else {
                        generic += ":" + advance().lexeme;
                    }
                }
                function.generic_parameters.push_back(generic);
            } else {
                add_error("XPARSE0010", "Expected a generic parameter name", peek().span);
                advance();
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        expect(TokenKind::RightBracket, "Expected ']' after generic parameters");
    }

    if (!expect(TokenKind::LeftParen, "Expected '(' after function name")) {
        synchronize_top_level();
        return true;
    }
    parse_parameter_list(function);

    if (match_keyword("returns")) {
        function.return_type = parse_type_name();
        if (function.return_type.empty()) {
            add_error("XPARSE0011", "Expected a return type after 'returns'", peek().span);
        }
    }

    if (check(TokenKind::LeftBrace)) {
        function.has_body = true;
        function.body = parse_block();
    } else if (match(TokenKind::Semicolon)) {
        function.has_body = false;
    } else {
        add_error("XPARSE0012", "Expected a function body or ';' after function signature", peek().span);
        synchronize_top_level();
    }

    const SourcePosition end = previous().span.end;
    function.span = {start, end, tokens_[start_index].span.source_id};
    result_.unit.functions.push_back(std::move(function));
    return true;
}

bool Parser::parse_parameter_list(FunctionDeclaration& function) {
    if (match(TokenKind::RightParen)) {
        return true;
    }
    while (!at_end()) {
        if (!parse_parameter(function)) {
            synchronize_statement();
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        if (match(TokenKind::Comma)) {
            continue;
        }
        break;
    }
    return expect(TokenKind::RightParen, "Expected ')' after function parameters");
}

bool Parser::parse_parameter(FunctionDeclaration& function) {
    Parameter parameter;
    const SourceSpan start_span = peek().span;
    const SourcePosition start = start_span.start;
    if (check_keyword("borrow") || check_keyword("take") || check_keyword("copy")) {
        parameter.passing_mode = lower_ascii(advance().lexeme);
        if (parameter.passing_mode == "borrow" && match_keyword("mut")) {
            parameter.passing_mode = "borrow mut";
        }
    }
    parameter.type_name = parse_type_name();
    if (parameter.type_name.empty()) {
        add_error("XPARSE0013", "Expected parameter type", peek().span);
        return false;
    }
    if (peek().kind != TokenKind::Identifier) {
        add_error("XPARSE0014", "Expected parameter name after its type", peek().span);
        return false;
    }
    parameter.name = advance().lexeme;
    if (match_operator("=")) {
        parameter.default_value = parse_expression();
        if (!parameter.default_value) {
            add_error("XPARSE0015", "Expected a default argument expression", peek().span);
        }
    }
    parameter.span = {start, previous().span.end, start_span.source_id};
    function.parameters.push_back(std::move(parameter));
    return true;
}

std::string Parser::parse_type_name() {
    std::string type;
    if (match_keyword("borrow")) {
        type = "borrow ";
        if (match_keyword("mut")) {
            type += "mut ";
        }
    } else if (match_keyword("take") || match_keyword("copy")) {
        type = previous().lexeme + " ";
    }
    while (check_operator("*")) {
        type += advance().lexeme;
    }
    if (!is_name_token(peek())) {
        return {};
    }
    type += advance().lexeme;
    while (true) {
        if (match(TokenKind::Dot)) {
            if (!is_name_token(peek())) {
                add_error("XPARSE0016", "Expected a type name after '.'", peek().span);
                break;
            }
            type += "." + advance().lexeme;
            continue;
        }
        if (match(TokenKind::LeftBracket)) {
            type += "[";
            bool first = true;
            while (!at_end() && !check(TokenKind::RightBracket)) {
                const std::string nested = parse_type_name();
                if (nested.empty()) {
                    add_error("XPARSE0017", "Expected a generic type argument", peek().span);
                    break;
                }
                if (!first) {
                    type += ",";
                }
                first = false;
                type += nested;
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            expect(TokenKind::RightBracket, "Expected ']' after generic type arguments");
            type += "]";
            continue;
        }
        break;
    }
    while (check_operator("*") || check_operator("?")) {
        type += advance().lexeme;
    }
    return type;
}

std::unique_ptr<Statement> Parser::parse_block() {
    if (!expect(TokenKind::LeftBrace, "Expected '{' to begin block")) {
        return nullptr;
    }
    auto block = std::make_unique<Statement>();
    block->kind = StatementKind::Block;
    const SourceSpan start_span = previous().span;
    const SourcePosition start = start_span.start;
    while (!at_end() && !check(TokenKind::RightBrace)) {
        const std::size_t before = current_;
        auto statement = parse_statement();
        if (statement) {
            block->statements.push_back(std::move(statement));
        }
        if (current_ == before) {
            advance();
        }
    }
    if (expect(TokenKind::RightBrace, "Expected '}' to close block")) {
        block->span = {start, previous().span.end, start_span.source_id};
    } else {
        block->span = {start, peek().span.end, start_span.source_id};
    }
    return block;
}

std::unique_ptr<Statement> Parser::parse_statement() {
    if (check(TokenKind::LeftBrace)) {
        return parse_block();
    }
    if (match_keyword("if")) {
        return parse_if_statement();
    }
    if (match_keyword("while")) {
        return parse_while_statement();
    }
    if (match_keyword("for")) {
        return parse_for_statement();
    }
    if (match_keyword("return")) {
        return parse_return_statement();
    }
    if (match_keyword("break")) {
        auto statement = std::make_unique<Statement>();
        statement->kind = StatementKind::Break;
        statement->span.start = previous().span.start;
        statement->span.source_id = previous().span.source_id;
        expect(TokenKind::Semicolon, "Expected ';' after break");
        statement->span.end = previous().span.end;
        return statement;
    }
    if (match_keyword("continue")) {
        auto statement = std::make_unique<Statement>();
        statement->kind = StatementKind::Continue;
        statement->span.start = previous().span.start;
        statement->span.source_id = previous().span.source_id;
        expect(TokenKind::Semicolon, "Expected ';' after continue");
        statement->span.end = previous().span.end;
        return statement;
    }
    if (looks_like_command_call()) {
        auto statement = std::make_unique<Statement>();
        statement->kind = StatementKind::Expression;
        statement->span.start = peek().span.start;
        statement->span.source_id = peek().span.source_id;
        auto expression = parse_command_call();
        if (expression) {
            statement->expressions.push_back(std::move(expression));
        }
        expect(TokenKind::Semicolon, "Expected ';' after command-style call");
        statement->span.end = previous().span.end;
        return statement;
    }
    if (looks_like_typed_declaration()) {
        return parse_variable_declaration();
    }
    return parse_expression_statement();
}

std::unique_ptr<Statement> Parser::parse_if_statement() {
    auto statement = std::make_unique<Statement>();
    statement->kind = StatementKind::If;
    statement->span.start = previous().span.start;
    statement->span.source_id = previous().span.source_id;
    expect(TokenKind::LeftParen, "Expected '(' after if");
    auto condition = parse_expression();
    if (condition) {
        statement->expressions.push_back(std::move(condition));
    } else {
        add_error("XPARSE0030", "Expected an if condition expression", peek().span);
    }
    expect(TokenKind::RightParen, "Expected ')' after if condition");
    auto then_block = parse_block();
    if (then_block) {
        statement->statements.push_back(std::move(then_block));
    }
    if (match_keyword("else")) {
        if (match_keyword("if")) {
            statement->else_branch = parse_if_statement();
        } else {
            statement->else_branch = parse_block();
        }
    }
    statement->span.end = previous().span.end;
    return statement;
}

std::unique_ptr<Statement> Parser::parse_while_statement() {
    auto statement = std::make_unique<Statement>();
    statement->kind = StatementKind::While;
    statement->span.start = previous().span.start;
    statement->span.source_id = previous().span.source_id;
    expect(TokenKind::LeftParen, "Expected '(' after while");
    auto condition = parse_expression();
    if (condition) {
        statement->expressions.push_back(std::move(condition));
    } else {
        add_error("XPARSE0031", "Expected a while condition expression", peek().span);
    }
    expect(TokenKind::RightParen, "Expected ')' after while condition");
    auto body = parse_block();
    if (body) {
        statement->statements.push_back(std::move(body));
    }
    statement->span.end = previous().span.end;
    return statement;
}

std::unique_ptr<Statement> Parser::parse_for_statement() {
    auto statement = std::make_unique<Statement>();
    statement->kind = StatementKind::ForEach;
    statement->span.start = previous().span.start;
    statement->span.source_id = previous().span.source_id;
    const bool parenthesized = match(TokenKind::LeftParen);
    if (peek().kind != TokenKind::Identifier) {
        add_error("XPARSE0018", "Expected loop variable after 'for'", peek().span);
    } else {
        statement->name = advance().lexeme;
    }
    if (!match_keyword("in")) {
        add_error("XPARSE0019", "Expected 'in' after loop variable", peek().span);
    }
    auto sequence = parse_expression();
    if (sequence) {
        statement->expressions.push_back(std::move(sequence));
    } else {
        add_error("XPARSE0032", "Expected an iterable expression after 'in'", peek().span);
    }
    if (parenthesized) {
        expect(TokenKind::RightParen, "Expected ')' after for-in header");
    }
    auto body = parse_block();
    if (body) {
        statement->statements.push_back(std::move(body));
    }
    statement->span.end = previous().span.end;
    return statement;
}

std::unique_ptr<Statement> Parser::parse_return_statement() {
    auto statement = std::make_unique<Statement>();
    statement->kind = StatementKind::Return;
    statement->span.start = previous().span.start;
    statement->span.source_id = previous().span.source_id;
    if (!check(TokenKind::Semicolon)) {
        auto expression = parse_expression();
        if (expression) {
            statement->expressions.push_back(std::move(expression));
        }
    }
    expect(TokenKind::Semicolon, "Expected ';' after return");
    statement->span.end = previous().span.end;
    return statement;
}

bool Parser::looks_like_typed_declaration() const {
    std::size_t index = current_;
    if (tokens_[index].kind == TokenKind::Keyword &&
        (lower_ascii(tokens_[index].lexeme) == "borrow" ||
         lower_ascii(tokens_[index].lexeme) == "take" ||
         lower_ascii(tokens_[index].lexeme) == "copy")) {
        ++index;
        if (index < tokens_.size() && tokens_[index].kind == TokenKind::Keyword &&
            lower_ascii(tokens_[index].lexeme) == "mut") {
            ++index;
        }
    }
    while (index < tokens_.size() && tokens_[index].kind == TokenKind::Operator &&
           tokens_[index].lexeme == "*") {
        ++index;
    }
    if (index >= tokens_.size() || !is_name_token(tokens_[index])) {
        return false;
    }
    ++index;
    while (index < tokens_.size()) {
        if (tokens_[index].kind == TokenKind::Dot && index + 1 < tokens_.size() &&
            is_name_token(tokens_[index + 1])) {
            index += 2;
            continue;
        }
        if (tokens_[index].kind == TokenKind::LeftBracket) {
            int depth = 1;
            ++index;
            while (index < tokens_.size() && depth > 0) {
                if (tokens_[index].kind == TokenKind::LeftBracket) ++depth;
                if (tokens_[index].kind == TokenKind::RightBracket) --depth;
                ++index;
            }
            continue;
        }
        if (tokens_[index].kind == TokenKind::Operator &&
            (tokens_[index].lexeme == "*" || tokens_[index].lexeme == "?")) {
            ++index;
            continue;
        }
        break;
    }
    return index < tokens_.size() && tokens_[index].kind == TokenKind::Identifier;
}

bool Parser::looks_like_command_call() const {
    if (peek().kind != TokenKind::Identifier || peek(1).kind == TokenKind::Operator ||
        peek(1).kind == TokenKind::LeftParen || peek(1).kind == TokenKind::Semicolon ||
        peek(1).kind == TokenKind::Dot || peek(1).kind == TokenKind::LeftBracket) {
        return false;
    }
    const std::string callee = lower_ascii(peek().lexeme);
    const bool prelude_command = callee == "print" || callee == "printline" ||
                                 callee == "input" || callee == "exit" || callee == "assert";
    return prelude_command && begins_expression(peek(1));
}

std::unique_ptr<Statement> Parser::parse_variable_declaration() {
    auto statement = std::make_unique<Statement>();
    statement->kind = StatementKind::VariableDeclaration;
    statement->explicit_type = true;
    statement->span.start = peek().span.start;
    statement->span.source_id = peek().span.source_id;
    statement->type_name = parse_type_name();
    if (peek().kind != TokenKind::Identifier) {
        add_error("XPARSE0020", "Expected variable name after its type", peek().span);
    } else {
        statement->name = advance().lexeme;
    }
    if (match_operator("=")) {
        auto initializer = parse_expression();
        if (initializer) {
            statement->expressions.push_back(std::move(initializer));
        } else {
            add_error("XPARSE0021", "Expected initializer expression", peek().span);
        }
    }
    expect(TokenKind::Semicolon, "Expected ';' after variable declaration");
    statement->span.end = previous().span.end;
    return statement;
}

std::unique_ptr<Statement> Parser::parse_expression_statement() {
    auto statement = std::make_unique<Statement>();
    statement->span.start = peek().span.start;
    statement->span.source_id = peek().span.source_id;
    auto expression = parse_expression();
    if (!expression) {
        add_error("XPARSE0022", "Expected a statement expression", peek().span);
        synchronize_statement();
        if (match(TokenKind::Semicolon)) {
            statement->span.end = previous().span.end;
        }
        return statement;
    }
    if (match_operator("=")) {
        statement->kind = StatementKind::Assignment;
        statement->operation = "=";
        statement->expressions.push_back(std::move(expression));
        auto value = parse_expression();
        if (value) {
            statement->expressions.push_back(std::move(value));
        } else {
            add_error("XPARSE0023", "Expected assignment value", peek().span);
        }
        expect(TokenKind::Semicolon, "Expected ';' after assignment");
    } else {
        statement->kind = StatementKind::Expression;
        statement->expressions.push_back(std::move(expression));
        expect(TokenKind::Semicolon, "Expected ';' after expression");
    }
    statement->span.end = previous().span.end;
    return statement;
}

std::unique_ptr<Expression> Parser::parse_expression(int min_precedence) {
    auto left = parse_prefix_expression();
    if (!left) {
        return nullptr;
    }
    while (true) {
        const int precedence = operator_precedence(peek());
        if (precedence < min_precedence) {
            break;
        }
        const Token operation = advance();
        auto right = parse_expression(precedence + 1);
        if (!right) {
            add_error("XPARSE0024", "Expected expression after binary operator", peek().span);
            break;
        }
        auto combined = std::make_unique<Expression>();
        combined->kind = ExprKind::Binary;
        combined->text = operation.lexeme;
        combined->span = joined_span(left->span, right->span);
        combined->children.push_back(std::move(left));
        combined->children.push_back(std::move(right));
        left = std::move(combined);
    }
    return left;
}

std::unique_ptr<Expression> Parser::parse_prefix_expression() {
    if (is_prefix_operator(peek())) {
        const Token operation = advance();
        auto operand = parse_prefix_expression();
        if (!operand) {
            add_error("XPARSE0025", "Expected an operand after unary operator", peek().span);
            return nullptr;
        }
        auto expression = std::make_unique<Expression>();
        expression->kind = ExprKind::Unary;
        expression->text = operation.lexeme;
        expression->span = joined_span(operation.span, operand->span);
        expression->children.push_back(std::move(operand));
        return expression;
    }
    return parse_postfix_expression(parse_primary_expression());
}

std::unique_ptr<Expression> Parser::parse_primary_expression() {
    if (peek().kind == TokenKind::IntegerLiteral || peek().kind == TokenKind::FloatingLiteral ||
        peek().kind == TokenKind::StringLiteral) {
        const Token value = advance();
        auto expression = std::make_unique<Expression>();
        expression->kind = ExprKind::Literal;
        expression->text = value.lexeme;
        expression->span = value.span;
        return expression;
    }
    if (peek().kind == TokenKind::Identifier || check_keyword("true") ||
        check_keyword("false") || check_keyword("null")) {
        const Token value = advance();
        auto expression = std::make_unique<Expression>();
        expression->kind = (value.kind == TokenKind::Identifier) ? ExprKind::Identifier : ExprKind::Literal;
        expression->text = value.lexeme;
        expression->span = value.span;
        return expression;
    }
    if (match(TokenKind::LeftParen)) {
        const Token start = previous();
        auto expression = parse_expression();
        expect(TokenKind::RightParen, "Expected ')' after expression");
        if (expression) {
            expression->span = joined_span(start.span, previous().span);
        }
        return expression;
    }
    if (match(TokenKind::LeftBracket)) {
        const Token start = previous();
        auto expression = std::make_unique<Expression>();
        expression->kind = ExprKind::List;
        if (!check(TokenKind::RightBracket)) {
            do {
                auto item = parse_expression();
                if (!item) {
                    add_error("XPARSE0026", "Expected list element expression", peek().span);
                    break;
                }
                expression->children.push_back(std::move(item));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RightBracket, "Expected ']' after list literal");
        expression->span = joined_span(start.span, previous().span);
        return expression;
    }
    return nullptr;
}

std::unique_ptr<Expression> Parser::parse_postfix_expression(std::unique_ptr<Expression> expression) {
    if (!expression) {
        return nullptr;
    }
    while (true) {
        if (match(TokenKind::Dot)) {
            // A member name is an identifier in this syntactic position even
            // when its spelling is reserved elsewhere (for example .Take()).
            if (!is_name_token(peek())) {
                add_error("XPARSE0027", "Expected member name after '.'", peek().span);
                return expression;
            }
            const Token member = advance();
            auto access = std::make_unique<Expression>();
            access->kind = ExprKind::Member;
            access->text = member.lexeme;
            access->span = joined_span(expression->span, member.span);
            access->children.push_back(std::move(expression));
            expression = std::move(access);
            continue;
        }
        if (match(TokenKind::LeftParen)) {
            auto call = std::make_unique<Expression>();
            call->kind = ExprKind::Call;
            call->span.start = expression->span.start;
            call->span.source_id = expression->span.source_id;
            call->children.push_back(std::move(expression));
            if (!check(TokenKind::RightParen)) {
                do {
                    auto argument = parse_expression();
                    if (!argument) {
                        add_error("XPARSE0028", "Expected call argument", peek().span);
                        break;
                    }
                    call->children.push_back(std::move(argument));
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RightParen, "Expected ')' after call arguments");
            call->span.end = previous().span.end;
            expression = std::move(call);
            continue;
        }
        if (match(TokenKind::LeftBracket)) {
            auto index = std::make_unique<Expression>();
            index->kind = ExprKind::Index;
            index->span.start = expression->span.start;
            index->span.source_id = expression->span.source_id;
            index->children.push_back(std::move(expression));
            auto subscript = parse_expression();
            if (subscript) {
                index->children.push_back(std::move(subscript));
            } else {
                add_error("XPARSE0029", "Expected index expression", peek().span);
            }
            expect(TokenKind::RightBracket, "Expected ']' after index expression");
            index->span.end = previous().span.end;
            expression = std::move(index);
            continue;
        }
        break;
    }
    return expression;
}

std::unique_ptr<Expression> Parser::parse_command_call() {
    const Token callee = advance();
    auto expression = std::make_unique<Expression>();
    expression->kind = ExprKind::Call;
    expression->span.start = callee.span.start;
    expression->span.source_id = callee.span.source_id;
    auto name = std::make_unique<Expression>();
    name->kind = ExprKind::Identifier;
    name->text = callee.lexeme;
    name->span = callee.span;
    expression->children.push_back(std::move(name));
    while (!at_end() && !check(TokenKind::Semicolon)) {
        auto argument = parse_expression();
        if (!argument) {
            break;
        }
        expression->children.push_back(std::move(argument));
        if (!match(TokenKind::Comma)) {
            break;
        }
    }
    expression->span.end = previous().span.end;
    return expression;
}

int Parser::operator_precedence(const Token& token) const {
    if (token.kind != TokenKind::Operator) {
        return 0;
    }
    const std::string& op = token.lexeme;
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "|") return 3;
    if (op == "^") return 4;
    if (op == "&") return 5;
    if (op == "==" || op == "!=") return 6;
    if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "..") return 7;
    if (op == "<<" || op == ">>") return 8;
    if (op == "+" || op == "-") return 9;
    if (op == "*" || op == "/" || op == "%") return 10;
    return 0;
}

SourceSpan Parser::joined_span(const SourceSpan& first, const SourceSpan& last) const {
    return SourceSpan{first.start, last.end, first.source_id};
}

bool Parser::check_operator(const char* operation) const {
    return peek().kind == TokenKind::Operator && peek().lexeme == operation;
}

bool Parser::match_operator(const char* operation) {
    if (!check_operator(operation)) {
        return false;
    }
    advance();
    return true;
}

ParseResult parse_source(const std::string& source) {
    LexResult lexical = lex_source(source);
    Parser parser(std::move(lexical.tokens));
    ParseResult parsed = parser.run();
    if (!lexical.diagnostics.empty()) {
        parsed.diagnostics.insert(parsed.diagnostics.begin(), lexical.diagnostics.begin(),
                                  lexical.diagnostics.end());
    }
    return parsed;
}

ParseResult parse_source(const SourceManager& sources, SourceFileId source_id) {
    LexResult lexical = lex_source(sources, source_id);
    Parser parser(std::move(lexical.tokens));
    ParseResult parsed = parser.run();
    if (!lexical.diagnostics.empty()) {
        parsed.diagnostics.insert(parsed.diagnostics.begin(), lexical.diagnostics.begin(),
                                  lexical.diagnostics.end());
    }
    return parsed;
}

} // namespace xfrontend
