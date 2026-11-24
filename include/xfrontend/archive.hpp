// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/object_file.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xfrontend {

enum class ArchiveFormat : std::uint8_t { GnuAr = 1U, CoffLib = 2U };

struct ArchiveSymbolIndexEntry {
    std::string symbol_name;
    std::size_t member_index = 0U;
};

struct ArchiveContainerMember {
    std::string name;
    std::vector<std::uint8_t> contents;
    std::vector<std::string> defined_symbols;
    std::uint64_t header_offset = 0U;
};

struct ArchiveContainer {
    ArchiveFormat format = ArchiveFormat::GnuAr;
    std::vector<ArchiveContainerMember> members;
    std::vector<ArchiveSymbolIndexEntry> symbol_index;
    bool has_symbol_index = false;
};

struct ArchiveWriteResult {
    std::vector<std::uint8_t> bytes;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && bytes.size() >= 8U; }
};

struct ArchiveReadResult {
    ArchiveContainer archive;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

struct ObjectArchiveWriteResult {
    std::vector<std::uint8_t> bytes;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && bytes.size() >= 8U; }
};

struct ObjectArchiveReadResult {
    StaticArchive archive;
    ArchiveFormat format = ArchiveFormat::GnuAr;
    std::vector<ArchiveSymbolIndexEntry> symbol_index;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Writes standard System V/GNU ar or Microsoft COFF LIB containers. Both
// formats use the standard ar header framing; their symbol indexes differ.
ArchiveWriteResult write_archive_container(ArchiveFormat format,
    const std::vector<ArchiveContainerMember>& members, bool include_symbol_index = true);
ArchiveReadResult read_archive_container(const std::vector<std::uint8_t>& bytes);

// Object-aware helpers encode and decode members through the N23 ELF64/AMD64
// COFF object reader/writer. Archive member bytes are validated before linking.
ObjectArchiveWriteResult write_object_archive(ArchiveFormat format,
    const std::vector<std::pair<std::string, ObjectFile>>& objects,
    bool include_symbol_index = true);
ObjectArchiveReadResult read_object_archive(const std::vector<std::uint8_t>& bytes,
    const std::string& archive_id);

} // namespace xfrontend
