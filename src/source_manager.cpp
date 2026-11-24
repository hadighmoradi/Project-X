// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/source_manager.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace xfrontend {
namespace {

struct Decoded {
    std::uint32_t value = 0;
    std::size_t length = 1;
    bool valid = true;
};

Decoded decode_utf8(const std::string& text, std::size_t offset) noexcept {
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

bool has_leading_bom(const std::string& text) noexcept {
    return text.size() >= 3 &&
           static_cast<unsigned char>(text[0]) == 0xEFU &&
           static_cast<unsigned char>(text[1]) == 0xBBU &&
           static_cast<unsigned char>(text[2]) == 0xBFU;
}

#if defined(_WIN32)
std::string ascii_lower(std::string value) {
    for (char& character : value) {
        const unsigned char ch = static_cast<unsigned char>(character);
        if (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(ch - static_cast<unsigned char>('A') +
                                          static_cast<unsigned char>('a'));
        }
    }
    return value;
}
#endif

std::size_t newline_length(const std::string& text, std::size_t offset) noexcept {
    if (offset >= text.size()) {
        return 0;
    }
    const unsigned char ch = static_cast<unsigned char>(text[offset]);
    if (ch == static_cast<unsigned char>('\r')) {
        return offset + 1 < text.size() && text[offset + 1] == '\n' ? 2 : 1;
    }
    if (ch == static_cast<unsigned char>('\n')) {
        return 1;
    }
    if (offset + 2 < text.size() && ch == 0xE2U &&
        static_cast<unsigned char>(text[offset + 1]) == 0x80U &&
        (static_cast<unsigned char>(text[offset + 2]) == 0xA8U ||
         static_cast<unsigned char>(text[offset + 2]) == 0xA9U)) {
        return 3;
    }
    return 0;
}

} // namespace

SourceFileId SourceManager::next_id() const {
    if (files_.size() >= static_cast<std::size_t>(std::numeric_limits<SourceFileId>::max())) {
        throw std::overflow_error("SourceManager exhausted SourceFileId values");
    }
    return static_cast<SourceFileId>(files_.size() + 1U);
}

SourceFileId SourceManager::add_buffer(std::string display_name, std::string contents) {
    SourceFile file;
    file.id = next_id();
    file.display_name = display_name.empty()
        ? std::string("<memory:") + std::to_string(file.id) + ">"
        : std::move(display_name);
    file.contents = std::move(contents);
    rebuild_line_index(file);
    files_.push_back(std::move(file));
    return files_.back().id;
}

SourceLoadResult SourceManager::load_file(const std::filesystem::path& path) {
    SourceLoadResult result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.diagnostics.push_back(Diagnostic{
            "XSRC0001", "Unable to open source file: " + path_to_utf8(path), {}});
        return result;
    }

    std::string contents((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    if (stream.bad()) {
        result.diagnostics.push_back(Diagnostic{
            "XSRC0002", "Unable to read source file: " + path_to_utf8(path), {}});
        return result;
    }

    const std::string key = canonical_key(path);
    const auto existing = path_to_id_.find(key);
    if (existing != path_to_id_.end()) {
        const SourceFileId existing_id = existing->second;
        if (existing_id == InvalidSourceFileId ||
            static_cast<std::size_t>(existing_id) > files_.size()) {
            result.diagnostics.push_back(Diagnostic{
                "XSRC0003", "Source path index is inconsistent", {}});
            return result;
        }
        SourceFile* file = &files_[static_cast<std::size_t>(existing_id) - 1U];
        file->contents = std::move(contents);
        rebuild_line_index(*file);
        result.source_id = file->id;
        return result;
    }

    SourceFile file;
    file.id = next_id();
    file.display_name = path_to_utf8(path.filename());
    file.canonical_path = key;
    file.contents = std::move(contents);
    rebuild_line_index(file);
    files_.push_back(std::move(file));
    result.source_id = files_.back().id;
    path_to_id_.emplace(key, result.source_id);
    return result;
}

const SourceFile* SourceManager::get(SourceFileId id) const noexcept {
    if (id == InvalidSourceFileId || static_cast<std::size_t>(id) > files_.size()) {
        return nullptr;
    }
    return &files_[static_cast<std::size_t>(id) - 1U];
}

std::optional<SourceFileId> SourceManager::find_file(const std::filesystem::path& path) const {
    const auto found = path_to_id_.find(canonical_key(path));
    if (found == path_to_id_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<SourcePosition> SourceManager::position_at(
    SourceFileId id, std::size_t byte_offset) const noexcept {
    const SourceFile* file = get(id);
    if (file == nullptr) {
        return std::nullopt;
    }

    const std::size_t clamped = std::min(byte_offset, file->contents.size());
    const auto upper = std::upper_bound(file->line_start_offsets.begin(),
                                        file->line_start_offsets.end(), clamped);
    const std::size_t line_index = upper == file->line_start_offsets.begin()
        ? 0U : static_cast<std::size_t>(upper - file->line_start_offsets.begin() - 1);
    const std::size_t line_start = file->line_start_offsets[line_index];

    std::size_t visible_start = line_start;
    if (line_index == 0U && has_leading_bom(file->contents)) {
        visible_start = 3U;
    }

    std::uint32_t column = 1U;
    std::size_t cursor = visible_start;
    while (cursor < clamped) {
        const Decoded decoded = decode_utf8(file->contents, cursor);
        const std::size_t step = decoded.length == 0U ? 1U : decoded.length;
        cursor += std::min(step, clamped - cursor);
        ++column;
    }

    SourcePosition position;
    position.byte_offset = clamped;
    position.line = static_cast<std::uint32_t>(line_index + 1U);
    position.column = column;
    return position;
}

std::optional<std::string> SourceManager::line_text(
    SourceFileId id, std::uint32_t one_based_line) const {
    const SourceFile* file = get(id);
    if (file == nullptr || one_based_line == 0U ||
        static_cast<std::size_t>(one_based_line) > file->line_start_offsets.size()) {
        return std::nullopt;
    }

    const std::size_t index = static_cast<std::size_t>(one_based_line) - 1U;
    std::size_t begin = file->line_start_offsets[index];
    if (index == 0U && has_leading_bom(file->contents)) {
        begin = 3U;
    }
    std::size_t end = index + 1U < file->line_start_offsets.size()
        ? file->line_start_offsets[index + 1U] : file->contents.size();
    // Strip the line terminator from this line's displayed text.
    if (end >= 3U && end <= file->contents.size() &&
        static_cast<unsigned char>(file->contents[end - 3U]) == 0xE2U &&
        static_cast<unsigned char>(file->contents[end - 2U]) == 0x80U &&
        (static_cast<unsigned char>(file->contents[end - 1U]) == 0xA8U ||
         static_cast<unsigned char>(file->contents[end - 1U]) == 0xA9U)) {
        end -= 3U;
    } else if (end >= 2U && file->contents[end - 2U] == '\r' &&
               file->contents[end - 1U] == '\n') {
        end -= 2U;
    } else if (end >= 1U && (file->contents[end - 1U] == '\r' ||
                             file->contents[end - 1U] == '\n')) {
        end -= 1U;
    }
    if (end < begin) {
        end = begin;
    }
    return file->contents.substr(begin, end - begin);
}

std::size_t SourceManager::line_count(SourceFileId id) const noexcept {
    const SourceFile* file = get(id);
    return file == nullptr ? 0U : file->line_start_offsets.size();
}

void SourceManager::rebuild_line_index(SourceFile& file) {
    file.line_start_offsets.clear();
    file.line_start_offsets.push_back(0U);
    std::size_t cursor = 0U;
    while (cursor < file.contents.size()) {
        const std::size_t newline = newline_length(file.contents, cursor);
        if (newline != 0U) {
            cursor += newline;
            file.line_start_offsets.push_back(cursor);
            continue;
        }
        const Decoded decoded = decode_utf8(file.contents, cursor);
        cursor += decoded.length == 0U ? 1U : decoded.length;
    }
}

std::string SourceManager::canonical_key(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    error.clear();
    std::filesystem::path normalized = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        normalized = absolute.lexically_normal();
    }
    std::string key = normalized.generic_u8string();
#if defined(_WIN32)
    key = ascii_lower(std::move(key));
#endif
    return key;
}

std::string SourceManager::path_to_utf8(const std::filesystem::path& path) {
    return path.u8string();
}

} // namespace xfrontend
