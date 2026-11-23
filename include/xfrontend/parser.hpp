#pragma once

#include "xfrontend/diagnostic.hpp"
#include "xfrontend/lexer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace xfrontend {

class SourceManager;

enum class ExprKind {
    Literal,
    Identifier,
    Unary,
    Binary,
    Call,
    Member,
    Index,
    List
};

struct Expression {
    ExprKind kind = ExprKind::Literal;
    std::string text;
    SourceSpan span;
    std::vector<std::unique_ptr<Expression>> children;
};

enum class StatementKind {
    Block,
    VariableDeclaration,
    Assignment,
    Expression,
    Return,
    If,
    While,
    ForEach,
    Break,
    Continue
};

struct Statement {
    StatementKind kind = StatementKind::Block;
    std::string name;
    std::string type_name;
    std::string operation;
    bool explicit_type = false;
    SourceSpan span;
    std::vector<std::unique_ptr<Expression>> expressions;
    std::vector<std::unique_ptr<Statement>> statements;
    std::unique_ptr<Statement> else_branch;
};

struct Parameter {
    std::string name;
    std::string type_name;
    std::string passing_mode;
    std::unique_ptr<Expression> default_value;
    SourceSpan span;
};

enum class Visibility {
    Private,
    Public,
    Internal
};

struct FunctionDeclaration {
    std::string name;
    Visibility visibility = Visibility::Private;
    std::vector<std::string> generic_parameters;
    std::vector<Parameter> parameters;
    std::string return_type;
    bool has_body = false;
    std::unique_ptr<Statement> body;
    SourceSpan span;
};

struct CompilationUnit {
    std::string module_name;
    std::vector<std::string> imports;
    std::vector<SourceSpan> import_spans;
    std::vector<FunctionDeclaration> functions;
};

struct ParseResult {
    CompilationUnit unit;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// The parser consumes the exact UTF-8-aware token stream produced by Lexer.
// It builds a syntax AST only; names and types are not resolved here.
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    ParseResult run();

private:
    std::vector<Token> tokens_;
    std::size_t current_ = 0;
    ParseResult result_;
    bool saw_function_ = false;

    const Token& peek(std::size_t lookahead = 0) const noexcept;
    const Token& previous() const noexcept;
    bool at_end() const noexcept;
    bool check(TokenKind kind) const noexcept;
    bool check_keyword(const char* keyword) const;
    bool check_operator(const char* operation) const;
    bool match_operator(const char* operation);
    bool match(TokenKind kind);
    bool match_keyword(const char* keyword);
    const Token& advance();
    bool expect(TokenKind kind, const char* message);
    bool expect_identifier(const char* message);
    void add_error(const char* code, const char* message, const SourceSpan& span);
    void synchronize_top_level();
    void synchronize_statement();

    void parse_module_and_imports();
    void parse_top_level_declarations();
    bool parse_function();
    bool parse_parameter_list(FunctionDeclaration& function);
    bool parse_parameter(FunctionDeclaration& function);
    std::string parse_type_name();
    std::string parse_qualified_name(const char* context);

    std::unique_ptr<Statement> parse_block();
    std::unique_ptr<Statement> parse_statement();
    std::unique_ptr<Statement> parse_if_statement();
    std::unique_ptr<Statement> parse_while_statement();
    std::unique_ptr<Statement> parse_for_statement();
    std::unique_ptr<Statement> parse_return_statement();
    std::unique_ptr<Statement> parse_variable_declaration();
    std::unique_ptr<Statement> parse_expression_statement();
    bool looks_like_typed_declaration() const;
    bool looks_like_command_call() const;

    std::unique_ptr<Expression> parse_expression(int min_precedence = 1);
    std::unique_ptr<Expression> parse_prefix_expression();
    std::unique_ptr<Expression> parse_primary_expression();
    std::unique_ptr<Expression> parse_postfix_expression(std::unique_ptr<Expression> expression);
    std::unique_ptr<Expression> parse_command_call();
    int operator_precedence(const Token& token) const;
    SourceSpan joined_span(const SourceSpan& first, const SourceSpan& last) const;
};

ParseResult parse_source(const std::string& source);
ParseResult parse_source(const SourceManager& sources, SourceFileId source_id);

} // namespace xfrontend
