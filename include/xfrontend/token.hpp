// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/diagnostic.hpp"

#include <string>

namespace xfrontend {

enum class TokenKind {
    EndOfFile,
    Identifier,
    Keyword,
    IntegerLiteral,
    FloatingLiteral,
    StringLiteral,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Comma,
    Semicolon,
    Colon,
    Dot,
    Operator
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string lexeme;
    SourceSpan span;
};

const char* token_kind_name(TokenKind kind) noexcept;

} // namespace xfrontend
