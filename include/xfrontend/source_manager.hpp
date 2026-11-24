// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/diagnostic.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xfrontend {

struct SourceFile {
    SourceFileId id = InvalidSourceFileId;
    std::string display_name;
    std::string canonical_path;
    std::string contents;
    std::vector<std::size_t> line_start_offsets;
};

struct SourceLoadResult {
    SourceFileId source_id = InvalidSourceFileId;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept {
        return source_id != InvalidSourceFileId && diagnostics.empty();
    }
};

// Owns immutable snapshots of source buffers and assigns stable IDs for the
// lifetime of this manager. Re-loading the same canonical disk path refreshes
// the buffer while retaining its SourceFileId.
class SourceManager {
public:
    SourceFileId add_buffer(std::string display_name, std::string contents);
    SourceLoadResult load_file(const std::filesystem::path& path);

    const SourceFile* get(SourceFileId id) const noexcept;
    std::optional<SourceFileId> find_file(const std::filesystem::path& path) const;
    std::optional<SourcePosition> position_at(SourceFileId id,
                                               std::size_t byte_offset) const noexcept;
    std::optional<std::string> line_text(SourceFileId id,
                                         std::uint32_t one_based_line) const;
    std::size_t line_count(SourceFileId id) const noexcept;
    std::size_t source_count() const noexcept { return files_.size(); }

private:
    std::vector<SourceFile> files_;
    std::unordered_map<std::string, SourceFileId> path_to_id_;

    SourceFileId next_id() const;
    static void rebuild_line_index(SourceFile& file);
    static std::string canonical_key(const std::filesystem::path& path);
    static std::string path_to_utf8(const std::filesystem::path& path);
};

} // namespace xfrontend
