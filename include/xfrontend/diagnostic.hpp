#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

using SourceFileId = std::uint32_t;
constexpr SourceFileId InvalidSourceFileId = 0;

struct SourcePosition {
    std::size_t byte_offset = 0;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

struct SourceSpan {
    SourcePosition start;
    SourcePosition end; // Exclusive.
    SourceFileId source_id = InvalidSourceFileId;
};

struct Diagnostic {
    std::string code;
    std::string message;
    SourceSpan span;
};

} // namespace xfrontend
