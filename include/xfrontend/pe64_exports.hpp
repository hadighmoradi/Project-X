// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/coff_import_library.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

// One PE32+ export slot. A non-empty name denotes an IMAGE_EXPORT_NAME_TABLE
// entry; an empty name denotes an ordinal-only export. Aliases are represented
// as multiple records carrying the same ordinal.
struct Pe64ExportSymbol {
    std::string name;
    std::uint32_t ordinal = 0U;
    std::uint16_t hint = 0U;
    std::uint32_t function_rva = 0U;
    bool forwarded = false;
    std::string forwarder;
};

struct Pe64ExportDirectory {
    std::string dll_name;
    std::uint64_t image_base = 0U;
    std::uint32_t directory_rva = 0U;
    std::uint32_t directory_size = 0U;
    std::uint32_t ordinal_base = 0U;
    std::vector<Pe64ExportSymbol> exports;
};

struct Pe64ExportReadResult {
    Pe64ExportDirectory directory;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && !directory.dll_name.empty(); }
};

// Optional metadata for exports whose storage kind cannot be inferred from
// the PE export address table. Match exactly one key: export_name OR ordinal.
// Unspecified exports are treated as CODE imports (the normal DLL-function case).
struct Pe64ExportKindOverride {
    std::string export_name;
    std::uint32_t ordinal = 0U;
    CoffImportType type = CoffImportType::Code;
};

struct Pe64ExportImportLibraryResult {
    std::vector<std::uint8_t> bytes;
    std::vector<Diagnostic> diagnostics;
    std::size_t import_count = 0U;
    bool succeeded() const noexcept { return diagnostics.empty() && bytes.size() >= 8U; }
};

// Reads and validates the PE32+ AMD64 Export Directory from a DLL image.
// The reader handles named exports, aliases, ordinal-only exports and forwarders.
Pe64ExportReadResult read_pe64_export_directory(const std::vector<std::uint8_t>& image);

// Reads a DLL's export table and converts it into an N32-compatible COFF
// Import Library. Export DATA/CONST classification is not encoded in the PE
// export table; callers may provide explicit kind overrides for those entries.
Pe64ExportImportLibraryResult write_coff_import_library_from_pe64_dll(
    const std::vector<std::uint8_t>& image,
    const std::vector<Pe64ExportKindOverride>& kind_overrides = {});

} // namespace xfrontend
