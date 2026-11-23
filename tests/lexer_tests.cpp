#include "xfrontend/lexer.hpp"
#include "test_report.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using xfrontend::LexResult;
using xfrontend::TokenKind;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const xfrontend::Token& token(const LexResult& result, std::size_t index) {
    require(index < result.tokens.size(), "token index is out of range");
    return result.tokens[index];
}

std::size_t count_kind(const LexResult& result, TokenKind kind) {
    std::size_t count = 0;
    for (const auto& item : result.tokens) {
        if (item.kind == kind) {
            ++count;
        }
    }
    return count;
}

bool has_diagnostic(const LexResult& result, const std::string& code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

void run_test(const char* name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

} // namespace

void run_lexer_tests(int& passed, int& failed) {
    run_test("L001_function_keyword_and_identifier", [&] {
        const auto result = xfrontend::lex_source("function Add() {}");
        require(result.succeeded(), "unexpected diagnostic");
        require(token(result, 0).kind == TokenKind::Keyword, "function must be a keyword");
        require(token(result, 1).kind == TokenKind::Identifier, "Add must be an identifier");
    }, passed, failed);

    run_test("L002_case_insensitive_keyword", [&] {
        const auto result = xfrontend::lex_source("FuNcTiOn Run() {}");
        require(token(result, 0).kind == TokenKind::Keyword, "keyword matching must ignore case");
        require(token(result, 0).lexeme == "FuNcTiOn", "original spelling must be preserved");
    }, passed, failed);

    run_test("L003_integer_literal", [&] {
        const auto result = xfrontend::lex_source("12345");
        require(token(result, 0).kind == TokenKind::IntegerLiteral, "integer token expected");
        require(token(result, 0).lexeme == "12345", "integer lexeme mismatch");
    }, passed, failed);

    run_test("L004_decimal_literal", [&] {
        const auto result = xfrontend::lex_source("12.50");
        require(token(result, 0).kind == TokenKind::FloatingLiteral, "float token expected");
    }, passed, failed);

    run_test("L005_exponent_literal", [&] {
        const auto result = xfrontend::lex_source("6.02e23");
        require(token(result, 0).kind == TokenKind::FloatingLiteral, "exponent float expected");
        require(token(result, 0).lexeme == "6.02e23", "exponent lexeme mismatch");
    }, passed, failed);

    run_test("L006_punctuation_tokens", [&] {
        const auto result = xfrontend::lex_source("(){},;:{}[]");
        require(count_kind(result, TokenKind::LeftParen) == 1, "left paren");
        require(count_kind(result, TokenKind::RightParen) == 1, "right paren");
        require(count_kind(result, TokenKind::LeftBrace) == 2, "left braces");
        require(count_kind(result, TokenKind::RightBrace) == 2, "right braces");
        require(count_kind(result, TokenKind::LeftBracket) == 1, "left bracket");
        require(count_kind(result, TokenKind::RightBracket) == 1, "right bracket");
        require(count_kind(result, TokenKind::Comma) == 1, "comma");
        require(count_kind(result, TokenKind::Semicolon) == 1, "semicolon");
        require(count_kind(result, TokenKind::Colon) == 1, "colon");
    }, passed, failed);

    run_test("L007_longest_match_operators", [&] {
        const auto result = xfrontend::lex_source("a==b && c!=d <= e");
        require(count_kind(result, TokenKind::Operator) == 4, "operator count mismatch");
        require(token(result, 1).lexeme == "==", "== must be one token");
        require(token(result, 3).lexeme == "&&", "&& must be one token");
        require(token(result, 5).lexeme == "!=", "!= must be one token");
    }, passed, failed);

    run_test("L008_line_comment_skipped", [&] {
        const auto result = xfrontend::lex_source("a; // comment\nb;");
        require(result.succeeded(), "comment should be accepted");
        require(count_kind(result, TokenKind::Identifier) == 2, "both identifiers expected");
    }, passed, failed);

    run_test("L009_persian_comment_skipped", [&] {
        const auto result = xfrontend::lex_source(u8"// توضیح فارسی { ; \nvalue = 1;");
        require(result.succeeded(), "Persian comment should be accepted");
        require(token(result, 0).lexeme == "value", "comment text must not become tokens");
    }, passed, failed);

    run_test("L010_persian_string_preserved", [&] {
        const auto result = xfrontend::lex_source(u8"name = \"سلام دنیا\";");
        require(result.succeeded(), "Persian string should be accepted");
        require(token(result, 2).kind == TokenKind::StringLiteral, "string token expected");
        require(token(result, 2).lexeme == u8"\"سلام دنیا\"", "UTF-8 bytes must be preserved");
    }, passed, failed);

    run_test("L011_utf8_bom_accepted", [&] {
        const std::string source = std::string("\xEF\xBB\xBF") + "function Start() {}";
        const auto result = xfrontend::lex_source(source);
        require(result.succeeded(), "leading BOM must be accepted");
        require(token(result, 0).lexeme == "function", "BOM must not become a token");
        require(token(result, 0).span.start.byte_offset == 3, "byte offset must include BOM bytes");
        require(token(result, 0).span.start.column == 1, "BOM must not affect visible column");
    }, passed, failed);

    run_test("L012_empty_source_has_only_eof", [&] {
        const auto result = xfrontend::lex_source("");
        require(result.succeeded(), "empty input should be valid lexically");
        require(result.tokens.size() == 1, "only EOF expected");
        require(token(result, 0).kind == TokenKind::EndOfFile, "EOF expected");
    }, passed, failed);

    run_test("L013_ascii_source_positions", [&] {
        const auto result = xfrontend::lex_source("x = 7;");
        require(token(result, 0).span.start.line == 1, "line should be 1");
        require(token(result, 0).span.start.column == 1, "identifier column should be 1");
        require(token(result, 2).span.start.column == 5, "number column should be 5");
        require(token(result, 2).span.end.column == 6, "exclusive end column should be 6");
    }, passed, failed);

    run_test("L014_crlf_line_mapping", [&] {
        const auto result = xfrontend::lex_source("a;\r\nb;");
        require(token(result, 2).span.start.line == 2, "second line expected");
        require(token(result, 2).span.start.column == 1, "column should reset after CRLF");
    }, passed, failed);

    run_test("L015_unicode_line_separator", [&] {
        const auto result = xfrontend::lex_source(u8"first;\u2028second;");
        require(result.succeeded(), "Unicode line separator should be accepted");
        require(token(result, 2).span.start.line == 2, "Unicode separator must increment line");
    }, passed, failed);

    run_test("L016_unterminated_string_diagnostic", [&] {
        const auto result = xfrontend::lex_source("name = \"unfinished");
        require(has_diagnostic(result, "XLEX0003"), "unterminated string diagnostic expected");
    }, passed, failed);

    run_test("L017_newline_in_string_diagnostic", [&] {
        const auto result = xfrontend::lex_source("name = \"line1\nline2\"");
        require(has_diagnostic(result, "XLEX0002"), "line break diagnostic expected");
        require(has_diagnostic(result, "XLEX0003"), "unterminated string diagnostic expected");
    }, passed, failed);

    run_test("L018_invalid_utf8_diagnostic", [&] {
        std::string source = "name = \"";
        source.push_back(static_cast<char>(0xFF));
        source += "\";";
        const auto result = xfrontend::lex_source(source);
        require(has_diagnostic(result, "XLEX0001"), "invalid UTF-8 diagnostic expected");
    }, passed, failed);

    run_test("L019_non_ascii_identifier_rejected_in_v1", [&] {
        const auto result = xfrontend::lex_source(u8"نام = 1;");
        require(!result.succeeded(), "non-ASCII identifier must not pass v1 lexical rules");
        require(has_diagnostic(result, "XLEX0004"), "unexpected character diagnostic expected");
    }, passed, failed);

    run_test("L020_empty_string_literal", [&] {
        const auto result = xfrontend::lex_source("\"\"");
        require(result.succeeded(), "empty string should be valid");
        require(token(result, 0).kind == TokenKind::StringLiteral, "string token expected");
        require(token(result, 0).lexeme == "\"\"", "empty string lexeme mismatch");
    }, passed, failed);

    run_test("L021_escaped_quote", [&] {
        const auto result = xfrontend::lex_source("\"say \\\"hello\\\"\"");
        require(result.succeeded(), "escaped quote should not terminate string");
        require(count_kind(result, TokenKind::StringLiteral) == 1, "one string expected");
    }, passed, failed);

    run_test("L022_escaped_backslash", [&] {
        const auto result = xfrontend::lex_source("\"path\\\\name\"");
        require(result.succeeded(), "escaped backslash should be accepted");
        require(token(result, 0).kind == TokenKind::StringLiteral, "string token expected");
    }, passed, failed);

    run_test("L023_number_then_identifier", [&] {
        const auto result = xfrontend::lex_source("12abc");
        require(token(result, 0).kind == TokenKind::IntegerLiteral, "integer prefix expected");
        require(token(result, 1).kind == TokenKind::Identifier, "identifier suffix expected");
    }, passed, failed);

    run_test("L024_range_operator_not_decimal", [&] {
        const auto result = xfrontend::lex_source("1..10");
        require(token(result, 0).kind == TokenKind::IntegerLiteral, "first integer expected");
        require(token(result, 1).kind == TokenKind::Operator, "range operator expected");
        require(token(result, 1).lexeme == "..", "range operator spelling mismatch");
        require(token(result, 2).kind == TokenKind::IntegerLiteral, "second integer expected");
    }, passed, failed);

    run_test("L025_keyword_matching_is_case_insensitive", [&] {
        const auto result = xfrontend::lex_source("RETURN true False PrintLine");
        require(token(result, 0).kind == TokenKind::Keyword, "RETURN keyword expected");
        require(token(result, 1).kind == TokenKind::Keyword, "true keyword expected");
        require(token(result, 2).kind == TokenKind::Keyword, "False keyword expected");
        require(token(result, 3).kind == TokenKind::Identifier, "PrintLine is an identifier");
    }, passed, failed);

    run_test("L026_nested_braces_are_individual_tokens", [&] {
        const auto result = xfrontend::lex_source("{{{}}}");
        require(count_kind(result, TokenKind::LeftBrace) == 3, "three opening braces expected");
        require(count_kind(result, TokenKind::RightBrace) == 3, "three closing braces expected");
    }, passed, failed);

    run_test("L027_single_dot_punctuation", [&] {
        const auto result = xfrontend::lex_source("Device.Name");
        require(token(result, 1).kind == TokenKind::Dot, "single dot expected");
    }, passed, failed);

    run_test("L028_unicode_scalar_columns", [&] {
        const auto result = xfrontend::lex_source(u8"\"آب\" x");
        require(token(result, 1).span.start.column == 6, "two Persian scalars occupy two columns");
    }, passed, failed);

    run_test("L029_comment_at_eof", [&] {
        const auto result = xfrontend::lex_source("x; // final comment");
        require(result.succeeded(), "comment at EOF should be valid");
        require(result.tokens.size() == 3, "identifier, semicolon and EOF expected");
    }, passed, failed);

    run_test("L030_comment_delimiters_inside_string", [&] {
        const auto result = xfrontend::lex_source("text = \"// not a comment\";");
        require(result.succeeded(), "comment delimiters inside string are ordinary text");
        require(count_kind(result, TokenKind::StringLiteral) == 1, "string expected");
    }, passed, failed);

    run_test("L031_malformed_exponent_is_not_swallowed", [&] {
        const auto result = xfrontend::lex_source("12e+");
        require(token(result, 0).kind == TokenKind::IntegerLiteral, "integer prefix expected");
        require(token(result, 1).kind == TokenKind::Identifier, "e should remain identifier");
        require(token(result, 2).kind == TokenKind::Operator, "plus should remain operator");
    }, passed, failed);

    run_test("L032_unicode_nonbreaking_space", [&] {
        const auto result = xfrontend::lex_source(u8"left\u00A0right");
        require(result.succeeded(), "Unicode whitespace should be accepted");
        require(count_kind(result, TokenKind::Identifier) == 2, "two identifiers expected");
    }, passed, failed);

    run_test("L033_unknown_ascii_character", [&] {
        const auto result = xfrontend::lex_source("a $ b");
        require(has_diagnostic(result, "XLEX0004"), "unknown character diagnostic expected");
    }, passed, failed);

    run_test("L034_string_span_uses_utf8_byte_offsets", [&] {
        const auto result = xfrontend::lex_source(u8"\"سلام\"");
        require(token(result, 0).span.end.byte_offset - token(result, 0).span.start.byte_offset == 10,
                "four two-byte Persian letters plus two quote bytes expected");
    }, passed, failed);

    run_test("L035_eof_position_is_exclusive", [&] {
        const auto result = xfrontend::lex_source("abc");
        const auto& eof = token(result, result.tokens.size() - 1);
        require(eof.kind == TokenKind::EndOfFile, "EOF expected");
        require(eof.span.start.byte_offset == 3, "EOF byte offset mismatch");
        require(eof.span.start.column == 4, "EOF column mismatch");
    }, passed, failed);

    run_test("L036_module_keywords", [&] {
        const auto result = xfrontend::lex_source("module Demo; import Core.IO;");
        require(token(result, 0).kind == TokenKind::Keyword, "module keyword expected");
        require(token(result, 3).kind == TokenKind::Keyword, "import keyword expected");
    }, passed, failed);

    run_test("L037_escape_sequence_keeps_original_lexeme", [&] {
        const auto result = xfrontend::lex_source("\"line\\nnext\"");
        require(result.succeeded(), "escaped n should be accepted");
        require(token(result, 0).lexeme == "\"line\\nnext\"", "raw lexeme must remain intact");
    }, passed, failed);

    run_test("L038_colons_remain_separate_punctuation", [&] {
        const auto result = xfrontend::lex_source("Name:Other::Member");
        require(token(result, 1).kind == TokenKind::Colon, "single colon expected");
        require(token(result, 3).kind == TokenKind::Colon, "first colon of pair expected");
        require(token(result, 4).kind == TokenKind::Colon, "second colon of pair expected");
    }, passed, failed);

    run_test("L039_integer_decimal_boundary", [&] {
        const auto result = xfrontend::lex_source("4. 4.0 .5");
        require(token(result, 0).kind == TokenKind::IntegerLiteral, "4. must start with integer token");
        require(token(result, 1).kind == TokenKind::Dot, "trailing dot remains punctuation");
        require(token(result, 2).kind == TokenKind::FloatingLiteral, "4.0 is floating");
        require(token(result, 3).kind == TokenKind::Dot, ".5 starts with a dot token");
        require(token(result, 4).kind == TokenKind::IntegerLiteral, "5 follows leading dot");
    }, passed, failed);

    run_test("L040_standalone_carriage_return_line_break", [&] {
        const auto result = xfrontend::lex_source("a;\rb;");
        require(token(result, 2).span.start.line == 2, "CR must start a new line");
    }, passed, failed);

    run_test("L041_interior_bom_is_not_ignored", [&] {
        const auto result = xfrontend::lex_source(std::string("a") + "\xEF\xBB\xBF" + "b");
        require(has_diagnostic(result, "XLEX0004"), "interior BOM must not be treated as file header");
    }, passed, failed);

    run_test("L042_invalid_utf8_in_comment_is_still_reported", [&] {
        std::string source = "// comment ";
        source.push_back(static_cast<char>(0xC0));
        source += "\nx;";
        const auto result = xfrontend::lex_source(source);
        require(has_diagnostic(result, "XLEX0001"), "UTF-8 validation must include comments");
    }, passed, failed);

}
