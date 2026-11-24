// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/object_file.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

enum class Elf64SharedSection : std::uint16_t {
    Text = 1U,
    ReadOnlyData = 2U,
    Data = 3U
};

enum class Elf64DynamicSymbolKind : std::uint8_t {
    Object = 1U,
    Function = 2U
};

enum class Elf64ImportBinding : std::uint8_t {
    Got = 1U,
    Plt = 2U
};

struct Elf64ExportedSymbol {
    std::string name;
    Elf64SharedSection section = Elf64SharedSection::Text;
    std::size_t value = 0U;
    std::size_t size = 0U;
    Elf64DynamicSymbolKind kind = Elf64DynamicSymbolKind::Function;
};

struct Elf64RelativeRelocation {
    // Offset of the pointer-sized relocation destination inside .data/.bss.
    std::size_t target_offset = 0U;
    // The image-relative address is computed from section + value. This is
    // serialized as the signed r_addend of R_X86_64_RELATIVE.
    Elf64SharedSection addend_section = Elf64SharedSection::Text;
    std::size_t addend_value = 0U;
};

struct Elf64ImportedSymbol {
    std::string name;
    Elf64DynamicSymbolKind kind = Elf64DynamicSymbolKind::Function;
    // GOT uses R_X86_64_GLOB_DAT; PLT uses a lazy-bindable JUMP_SLOT.
    Elf64ImportBinding binding = Elf64ImportBinding::Got;
};

struct Elf64GotPcRelative32 {
    // Offset of a four-byte RIP-relative displacement field inside .text.
    // The field is patched to address the imported symbol's GOT slot.
    std::size_t text_displacement_offset = 0U;
    std::string symbol_name;
};

struct Elf64PltPcRelative32 {
    // Offset of a four-byte rel32 field in a near CALL instruction in .text.
    // The field is patched to the imported function's PLT entry.
    std::size_t text_displacement_offset = 0U;
    std::string symbol_name;
};

struct Elf64SharedObjectSpec {
    std::string soname;
    std::size_t page_size = 4096U;
    std::size_t maximum_file_size = 512U * 1024U * 1024U;
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> read_only_data;
    std::vector<std::uint8_t> data;
    std::size_t bss_size = 0U;
    std::vector<Elf64ExportedSymbol> exports;
    std::vector<Elf64ImportedSymbol> imports;
    // DT_NEEDED dependencies. Each name must be a valid NUL-free SONAME.
    std::vector<std::string> needed_libraries;
    std::vector<Elf64GotPcRelative32> got_pc_relative32;
    std::vector<Elf64PltPcRelative32> plt_pc_relative32;
    std::vector<Elf64RelativeRelocation> relative_relocations;
};

struct Elf64DynamicEntry {
    std::int64_t tag = 0;
    std::uint64_t value = 0U;
};

struct Elf64SharedObjectImage {
    ObjectFileDescriptor descriptor;
    std::vector<std::uint8_t> bytes;
    std::vector<Elf64DynamicEntry> dynamic_entries;
    std::uint64_t text_address = 0U;
    std::uint64_t read_only_data_address = 0U;
    std::uint64_t data_address = 0U;
    std::uint64_t dynamic_address = 0U;
    std::uint64_t relocation_address = 0U;
    std::size_t exported_symbol_count = 0U;
    std::size_t imported_symbol_count = 0U;
    std::size_t needed_library_count = 0U;
    std::size_t symbol_relocation_count = 0U;
    std::size_t relative_relocation_count = 0U;
    std::uint64_t got_address = 0U;
    std::size_t got_size = 0U;
    std::uint64_t gotplt_address = 0U;
    std::size_t gotplt_size = 0U;
    std::uint64_t plt_address = 0U;
    std::size_t plt_size = 0U;
    std::uint64_t plt_relocation_address = 0U;
    std::size_t plt_relocation_count = 0U;
    std::size_t memory_size = 0U;
};

struct Elf64SharedObjectResult {
    Elf64SharedObjectImage image;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && image.bytes.size() >= 64U; }
};

// Writes a Linux/x86-64 ELF64 ET_DYN shared object with exported and undefined
// imported dynsym entries, DT_NEEDED dependencies, SysV .hash, GOT references,
// R_X86_64_GLOB_DAT, and optional x86-64 PLT/GOTPLT with R_X86_64_JUMP_SLOT
// lazy-binding relocations. PLT call sites use rel32 CALL instructions supplied
// by the code generator. TLS, constructors, GNU hash, symbol versioning and
// GNU RELRO remain deliberately unsupported.
Elf64SharedObjectResult write_elf64_shared_object(const Elf64SharedObjectSpec& spec);

} // namespace xfrontend
