#pragma once

#include "xfrontend/diagnostic.hpp"
#include "xfrontend/token.hpp"

#include <string>
#include <vector>

namespace xfrontend {

class SourceManager;

struct LexResult {
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// C++17 reference implementation of the first production-oriented X lexer.
// Source spans use UTF-8 byte offsets and 1-based Unicode-scalar line/columns.
class Lexer {
public:
    explicit Lexer(std::string source, SourceFileId source_id = InvalidSourceFileId);
    LexResult run();

private:
    std::string source_;
    SourceFileId source_id_ = InvalidSourceFileId;
    std::size_t offset_ = 0;
    SourcePosition position_{};
    LexResult result_;

    void validate_utf8();
    void skip_initial_bom();
    void skip_trivia();
    void lex_identifier_or_keyword();
    void lex_number();
    void lex_string();
    void lex_operator_or_punctuation();

    void emit(TokenKind kind, const SourcePosition& start);
    void add_diagnostic(const char* code, const char* message,
                        const SourcePosition& start, const SourcePosition& end);

    std::uint32_t peek_code_point(std::size_t lookahead = 0) const noexcept;
    bool at_end() const noexcept;
    bool is_ascii_alpha(std::uint32_t value) const noexcept;
    bool is_ascii_digit(std::uint32_t value) const noexcept;
    bool is_identifier_start(std::uint32_t value) const noexcept;
    bool is_identifier_continue(std::uint32_t value) const noexcept;
    bool is_unicode_space(std::uint32_t value) const noexcept;
    bool is_line_break(std::uint32_t value) const noexcept;
    std::uint32_t advance();
    bool match_ascii(const char* text) const noexcept;
    static bool is_keyword(const std::string& text);
};

LexResult lex_source(const std::string& source);
LexResult lex_source(const SourceManager& sources, SourceFileId source_id);

} // namespace xfrontend
