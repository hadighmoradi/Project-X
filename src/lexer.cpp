#include "xfrontend/lexer.hpp"
#include "xfrontend/source_manager.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>

namespace xfrontend {
namespace {

struct DecodedCodePoint {
    std::uint32_t value = 0;
    std::size_t length = 1;
    bool valid = true;
};

DecodedCodePoint decode_utf8(const std::string& text, std::size_t offset) noexcept {
    if (offset >= text.size()) {
        return {0, 0, true};
    }

    const auto byte = [&text](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };

    const unsigned char first = byte(offset);
    if (first <= 0x7FU) {
        return {first, 1, true};
    }

    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
        length = 2;
        value = first & 0x1FU;
        minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        length = 3;
        value = first & 0x0FU;
        minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        length = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return {0xFFFDU, 1, false};
    }

    if (offset + length > text.size()) {
        return {0xFFFDU, 1, false};
    }

    for (std::size_t index = 1; index < length; ++index) {
        const unsigned char continuation = byte(offset + index);
        if ((continuation & 0xC0U) != 0x80U) {
            return {0xFFFDU, 1, false};
        }
        value = (value << 6U) | (continuation & 0x3FU);
    }

    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        return {0xFFFDU, 1, false};
    }

    return {value, length, true};
}

std::string ascii_lower(std::string text) {
    for (char& character : text) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value >= static_cast<unsigned char>('A') &&
            value <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(value - static_cast<unsigned char>('A') +
                                          static_cast<unsigned char>('a'));
        }
    }
    return text;
}

constexpr std::array<std::string_view, 8> kMultiCharacterOperators = {
    "==", "!=", "<=", ">=", "&&", "||", "<<", ">>"
};

} // namespace

const char* token_kind_name(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::EndOfFile: return "EndOfFile";
    case TokenKind::Identifier: return "Identifier";
    case TokenKind::Keyword: return "Keyword";
    case TokenKind::IntegerLiteral: return "IntegerLiteral";
    case TokenKind::FloatingLiteral: return "FloatingLiteral";
    case TokenKind::StringLiteral: return "StringLiteral";
    case TokenKind::LeftParen: return "LeftParen";
    case TokenKind::RightParen: return "RightParen";
    case TokenKind::LeftBrace: return "LeftBrace";
    case TokenKind::RightBrace: return "RightBrace";
    case TokenKind::LeftBracket: return "LeftBracket";
    case TokenKind::RightBracket: return "RightBracket";
    case TokenKind::Comma: return "Comma";
    case TokenKind::Semicolon: return "Semicolon";
    case TokenKind::Colon: return "Colon";
    case TokenKind::Dot: return "Dot";
    case TokenKind::Operator: return "Operator";
    }
    return "Unknown";
}

Lexer::Lexer(std::string source, SourceFileId source_id)
    : source_(std::move(source)), source_id_(source_id) {
    position_.byte_offset = 0;
    position_.line = 1;
    position_.column = 1;
    validate_utf8();
    skip_initial_bom();
}

LexResult Lexer::run() {
    while (!at_end()) {
        skip_trivia();
        if (at_end()) {
            break;
        }

        const std::uint32_t value = peek_code_point();
        if (is_identifier_start(value)) {
            lex_identifier_or_keyword();
        } else if (is_ascii_digit(value)) {
            lex_number();
        } else if (value == static_cast<std::uint32_t>('"')) {
            lex_string();
        } else {
            lex_operator_or_punctuation();
        }
    }

    const SourcePosition end = position_;
    result_.tokens.push_back(Token{TokenKind::EndOfFile, std::string(), {end, end, source_id_}});
    return std::move(result_);
}

void Lexer::validate_utf8() {
    std::size_t cursor = 0;
    SourcePosition location{};
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xEFU &&
        static_cast<unsigned char>(source_[1]) == 0xBBU &&
        static_cast<unsigned char>(source_[2]) == 0xBFU) {
        cursor = 3;
        location.byte_offset = 3;
    }

    while (cursor < source_.size()) {
        const DecodedCodePoint decoded = decode_utf8(source_, cursor);
        const SourcePosition start = location;
        if (!decoded.valid) {
            ++cursor;
            ++location.byte_offset;
            ++location.column;
            add_diagnostic("XLEX0001", "Invalid UTF-8 byte sequence", start, location);
            continue;
        }

        cursor += decoded.length;
        location.byte_offset += decoded.length;
        if (decoded.value == 0x000DU) {
            if (cursor < source_.size() && source_[cursor] == '\n') {
                ++cursor;
                ++location.byte_offset;
            }
            ++location.line;
            location.column = 1;
        } else if (decoded.value == 0x000AU || decoded.value == 0x2028U ||
                   decoded.value == 0x2029U) {
            ++location.line;
            location.column = 1;
        } else {
            ++location.column;
        }
    }
}

void Lexer::skip_initial_bom() {
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xEFU &&
        static_cast<unsigned char>(source_[1]) == 0xBBU &&
        static_cast<unsigned char>(source_[2]) == 0xBFU) {
        offset_ = 3;
        position_.byte_offset = 3;
    }
}

void Lexer::skip_trivia() {
    while (!at_end()) {
        const std::uint32_t value = peek_code_point();
        if (is_unicode_space(value)) {
            advance();
            continue;
        }

        if (value == static_cast<std::uint32_t>('/') &&
            offset_ + 1 < source_.size() && source_[offset_ + 1] == '/') {
            advance();
            advance();
            while (!at_end() && !is_line_break(peek_code_point())) {
                advance();
            }
            continue;
        }
        break;
    }
}

void Lexer::lex_identifier_or_keyword() {
    const SourcePosition start = position_;
    while (!at_end() && is_identifier_continue(peek_code_point())) {
        advance();
    }

    const std::string text = source_.substr(start.byte_offset,
                                             position_.byte_offset - start.byte_offset);
    emit(is_keyword(text) ? TokenKind::Keyword : TokenKind::Identifier, start);
}

void Lexer::lex_number() {
    const SourcePosition start = position_;
    while (!at_end() && is_ascii_digit(peek_code_point())) {
        advance();
    }

    bool is_floating = false;
    if (!at_end() && peek_code_point() == static_cast<std::uint32_t>('.') &&
        offset_ + 1 < source_.size() &&
        source_[offset_ + 1] >= '0' && source_[offset_ + 1] <= '9' &&
        !(offset_ + 1 < source_.size() && source_[offset_ + 1] == '.')) {
        is_floating = true;
        advance();
        while (!at_end() && is_ascii_digit(peek_code_point())) {
            advance();
        }
    }

    if (!at_end() && (peek_code_point() == static_cast<std::uint32_t>('e') ||
                      peek_code_point() == static_cast<std::uint32_t>('E'))) {
        const std::size_t exponent_offset = offset_;
        advance();
        if (!at_end() && (peek_code_point() == static_cast<std::uint32_t>('+') ||
                          peek_code_point() == static_cast<std::uint32_t>('-'))) {
            advance();
        }
        if (!at_end() && is_ascii_digit(peek_code_point())) {
            is_floating = true;
            while (!at_end() && is_ascii_digit(peek_code_point())) {
                advance();
            }
        } else {
            // A malformed exponent is left for the parser as separate tokens.
            offset_ = exponent_offset;
            position_.byte_offset = exponent_offset;
            // Restore the character column from the number's byte/code-point length.
            position_.column = start.column + static_cast<std::uint32_t>(exponent_offset - start.byte_offset);
        }
    }

    emit(is_floating ? TokenKind::FloatingLiteral : TokenKind::IntegerLiteral, start);
}

void Lexer::lex_string() {
    const SourcePosition start = position_;
    advance(); // Opening quote.
    bool terminated = false;

    while (!at_end()) {
        const std::uint32_t value = peek_code_point();
        if (value == static_cast<std::uint32_t>('"')) {
            advance();
            terminated = true;
            break;
        }
        if (is_line_break(value)) {
            add_diagnostic("XLEX0002", "Line break is not allowed inside a string literal",
                            start, position_);
            break;
        }
        if (value == static_cast<std::uint32_t>('\\')) {
            advance();
            if (!at_end() && !is_line_break(peek_code_point())) {
                advance();
            }
            continue;
        }
        advance();
    }

    if (!terminated) {
        add_diagnostic("XLEX0003", "Unterminated string literal", start, position_);
    }
    emit(TokenKind::StringLiteral, start);
}

void Lexer::lex_operator_or_punctuation() {
    const SourcePosition start = position_;
    const std::uint32_t value = peek_code_point();
    TokenKind kind = TokenKind::Operator;

    switch (value) {
    case '(': kind = TokenKind::LeftParen; advance(); emit(kind, start); return;
    case ')': kind = TokenKind::RightParen; advance(); emit(kind, start); return;
    case '{': kind = TokenKind::LeftBrace; advance(); emit(kind, start); return;
    case '}': kind = TokenKind::RightBrace; advance(); emit(kind, start); return;
    case '[': kind = TokenKind::LeftBracket; advance(); emit(kind, start); return;
    case ']': kind = TokenKind::RightBracket; advance(); emit(kind, start); return;
    case ',': kind = TokenKind::Comma; advance(); emit(kind, start); return;
    case ';': kind = TokenKind::Semicolon; advance(); emit(kind, start); return;
    case ':': kind = TokenKind::Colon; advance(); emit(kind, start); return;
    case '.':
        if (match_ascii("..")) {
            advance();
            advance();
        } else {
            advance();
            kind = TokenKind::Dot;
        }
        emit(kind, start);
        return;
    default:
        break;
    }

    constexpr const char* kSingleCharacterOperators = "+-*/%=!<>&|^~?";
    if (value < 0x80U &&
        std::string_view(kSingleCharacterOperators).find(static_cast<char>(value)) !=
            std::string_view::npos) {
        for (const std::string_view op : kMultiCharacterOperators) {
            if (match_ascii(op.data()) &&
                std::char_traits<char>::length(op.data()) == op.size()) {
                for (std::size_t index = 0; index < op.size(); ++index) {
                    advance();
                }
                emit(TokenKind::Operator, start);
                return;
            }
        }
        advance();
        emit(TokenKind::Operator, start);
        return;
    }

    const DecodedCodePoint decoded = decode_utf8(source_, offset_);
    advance();
    if (decoded.valid) {
        add_diagnostic("XLEX0004", "Unexpected character outside a string or comment",
                        start, position_);
    }
}

void Lexer::emit(TokenKind kind, const SourcePosition& start) {
    const std::size_t begin = start.byte_offset;
    const std::size_t length = position_.byte_offset - begin;
    result_.tokens.push_back(Token{kind, source_.substr(begin, length), {start, position_, source_id_}});
}

void Lexer::add_diagnostic(const char* code, const char* message,
                           const SourcePosition& start, const SourcePosition& end) {
    result_.diagnostics.push_back(Diagnostic{code, message, {start, end, source_id_}});
}

std::uint32_t Lexer::peek_code_point(std::size_t lookahead) const noexcept {
    std::size_t cursor = offset_;
    for (std::size_t index = 0; index < lookahead; ++index) {
        cursor += decode_utf8(source_, cursor).length;
        if (cursor >= source_.size()) {
            return 0;
        }
    }
    return decode_utf8(source_, cursor).value;
}

bool Lexer::at_end() const noexcept {
    return offset_ >= source_.size();
}

bool Lexer::is_ascii_alpha(std::uint32_t value) const noexcept {
    return (value >= static_cast<std::uint32_t>('a') && value <= static_cast<std::uint32_t>('z')) ||
           (value >= static_cast<std::uint32_t>('A') && value <= static_cast<std::uint32_t>('Z'));
}

bool Lexer::is_ascii_digit(std::uint32_t value) const noexcept {
    return value >= static_cast<std::uint32_t>('0') && value <= static_cast<std::uint32_t>('9');
}

bool Lexer::is_identifier_start(std::uint32_t value) const noexcept {
    return is_ascii_alpha(value) || value == static_cast<std::uint32_t>('_');
}

bool Lexer::is_identifier_continue(std::uint32_t value) const noexcept {
    return is_identifier_start(value) || is_ascii_digit(value);
}

bool Lexer::is_unicode_space(std::uint32_t value) const noexcept {
    return value == 0x0009U || value == 0x000AU || value == 0x000BU ||
           value == 0x000CU || value == 0x000DU || value == 0x0020U ||
           value == 0x0085U || value == 0x00A0U || value == 0x1680U ||
           (value >= 0x2000U && value <= 0x200AU) || value == 0x2028U ||
           value == 0x2029U || value == 0x202FU || value == 0x205FU ||
           value == 0x3000U;
}

bool Lexer::is_line_break(std::uint32_t value) const noexcept {
    return value == 0x000AU || value == 0x000DU || value == 0x2028U || value == 0x2029U;
}

std::uint32_t Lexer::advance() {
    if (at_end()) {
        return 0;
    }

    const DecodedCodePoint decoded = decode_utf8(source_, offset_);
    const std::uint32_t value = decoded.value;
    if (!decoded.valid) {
        ++offset_;
        ++position_.byte_offset;
        ++position_.column;
        return value;
    }

    offset_ += decoded.length;
    position_.byte_offset += decoded.length;
    if (value == 0x000DU) {
        if (offset_ < source_.size() && source_[offset_] == '\n') {
            ++offset_;
            ++position_.byte_offset;
        }
        ++position_.line;
        position_.column = 1;
    } else if (value == 0x000AU || value == 0x2028U || value == 0x2029U) {
        ++position_.line;
        position_.column = 1;
    } else {
        ++position_.column;
    }
    return value;
}

bool Lexer::match_ascii(const char* text) const noexcept {
    const std::size_t length = std::char_traits<char>::length(text);
    return offset_ + length <= source_.size() &&
           source_.compare(offset_, length, text, length) == 0;
}

bool Lexer::is_keyword(const std::string& text) {
    static const std::array<std::string_view, 19> keywords = {
        "function", "module", "import", "public", "private", "internal",
        "returns", "if", "else", "while", "for", "in", "return",
        "break", "continue", "true", "false", "null", "borrow"
    };
    const std::string folded = ascii_lower(text);
    if (std::find(keywords.begin(), keywords.end(), folded) != keywords.end()) {
        return true;
    }
    return folded == "mut" || folded == "take" || folded == "copy";
}

LexResult lex_source(const std::string& source) {
    return Lexer(source).run();
}

LexResult lex_source(const SourceManager& sources, SourceFileId source_id) {
    const SourceFile* file = sources.get(source_id);
    if (file == nullptr) {
        LexResult result;
        result.diagnostics.push_back(Diagnostic{
            "XSRC0004", "Unknown SourceFileId passed to lexer", {{}, {}, source_id}});
        return result;
    }
    return Lexer(file->contents, source_id).run();
}

} // namespace xfrontend
