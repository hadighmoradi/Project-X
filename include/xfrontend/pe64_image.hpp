// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/diagnostic.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

enum class Pe64SectionKind : std::uint8_t {
    Text = 1U,
    ReadOnlyData = 2U,
    Data = 3U
};

enum class Pe64ImportKind : std::uint8_t {
    ByName = 1U,
    ByOrdinal = 2U
};

enum class Pe64IatInstructionKind : std::uint8_t {
    CallIndirect = 1U, // FF 15 disp32
    JumpIndirect = 2U  // FF 25 disp32
};

struct Pe64Import {
    // DLL matching is ASCII case-insensitive; exported names remain exact.
    // ByOrdinal uses ordinal and requires an empty symbol_name.
    std::string dll_name;
    std::string symbol_name;
    std::uint16_t hint = 0U;
    Pe64ImportKind kind = Pe64ImportKind::ByName;
    std::uint16_t ordinal = 0U;
};

struct Pe64IatCallSite {
    // Offset of the 32-bit displacement in FF 15 (call) or FF 25 (jump).
    std::size_t displacement_offset = 0U;
    std::string dll_name;
    std::string symbol_name;
    Pe64IatInstructionKind instruction_kind = Pe64IatInstructionKind::CallIndirect;
    Pe64ImportKind import_kind = Pe64ImportKind::ByName;
    std::uint16_t ordinal = 0U;
};

struct Pe64AbsoluteRelocation {
    // The 64-bit pointer being relocated is stored in initialized .data.
    std::size_t destination_offset = 0U;
    Pe64SectionKind target_section = Pe64SectionKind::Data;
    std::size_t target_offset = 0U;
    std::int64_t addend = 0;
};

enum class Pe64UnwindOperation : std::uint8_t {
    PushNonVol = 0U, // UWOP_PUSH_NONVOL; operand is an AMD64 nonvolatile register number.
    AllocSmall = 2U  // UWOP_ALLOC_SMALL; operand is the stack allocation size in bytes.
};

struct Pe64UnwindCode {
    // Byte offset of the instruction end within the function prologue.
    std::uint8_t code_offset = 0U;
    Pe64UnwindOperation operation = Pe64UnwindOperation::AllocSmall;
    // AllocSmall: byte count (8..128, multiple of 8). PushNonVol: register id.
    std::uint16_t operand = 0U;
};

struct Pe64FunctionUnwind {
    std::size_t begin_offset = 0U;
    std::size_t end_offset = 0U; // exclusive, relative to .text
    std::uint8_t prolog_size = 0U;
    // The writer sorts codes by descending CodeOffset as required by x64 UNWIND_INFO.
    std::vector<Pe64UnwindCode> codes;
};

struct Pe64ExecutableSpec {
    std::uint64_t image_base = 0x140000000ULL;
    std::uint32_t section_alignment = 0x1000U;
    std::uint32_t file_alignment = 0x200U;
    std::size_t maximum_file_size = 512U * 1024U * 1024U;
    std::uint16_t subsystem = 3U; // IMAGE_SUBSYSTEM_WINDOWS_CUI
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> read_only_data;
    std::vector<std::uint8_t> data;
    std::size_t bss_size = 0U;
    std::size_t entry_point_offset = 0U;
    std::vector<Pe64Import> imports;
    std::vector<Pe64IatCallSite> iat_call_sites;
    std::vector<Pe64AbsoluteRelocation> absolute_relocations;
    std::vector<Pe64FunctionUnwind> function_unwind;
};

struct Pe64SectionInfo {
    std::string name;
    std::uint32_t virtual_address = 0U;
    std::uint32_t virtual_size = 0U;
    std::uint32_t raw_offset = 0U;
    std::uint32_t raw_size = 0U;
    std::uint32_t characteristics = 0U;
};

struct Pe64ExecutableImage {
    std::vector<std::uint8_t> bytes;
    std::vector<Pe64SectionInfo> sections;
    std::uint64_t image_base = 0U;
    std::uint32_t entry_point_rva = 0U;
    std::uint32_t size_of_image = 0U;
    std::uint32_t size_of_headers = 0U;
    std::uint32_t import_directory_rva = 0U;
    std::uint32_t import_directory_size = 0U;
    std::uint32_t iat_rva = 0U;
    std::uint32_t iat_size = 0U;
    std::uint32_t read_only_data_rva = 0U;
    std::uint32_t relocation_directory_rva = 0U;
    std::uint32_t relocation_directory_size = 0U;
    std::uint32_t exception_directory_rva = 0U;
    std::uint32_t exception_directory_size = 0U;
    std::uint32_t pdata_rva = 0U;
    std::uint32_t xdata_rva = 0U;
    std::size_t unwind_function_count = 0U;
    std::size_t imported_symbol_count = 0U;
    std::size_t imported_name_count = 0U;
    std::size_t imported_ordinal_count = 0U;
    std::size_t imported_library_count = 0U;
    std::size_t base_relocation_count = 0U;
    bool has_dynamic_base = false;
    bool has_nx_compat = true;
};

struct Pe64ExecutableResult {
    Pe64ExecutableImage image;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept {
        return diagnostics.empty() && image.bytes.size() >= 0x200U;
    }
};

// Emits a Windows AMD64 PE32+ executable image with .text/.rdata/.data,
// optional writable .idata and .reloc sections, multiple DLL descriptors,
// import-by-name and import-by-ordinal thunks, ILT/IAT, FF 15 call and FF 25
// jump patching, IMAGE_REL_BASED_DIR64 relocations, and AMD64 .pdata/.xdata
// UNWIND_INFO for UWOP_ALLOC_SMALL and UWOP_PUSH_NONVOL. N30 keeps direct
// process entry; CRT initialization, TLS, delay imports, forwarded exports,
// resources, other unwind opcodes and CFG are intentionally not implied.
Pe64ExecutableResult write_pe64_executable_image(const Pe64ExecutableSpec& spec);

// Builds a small, deterministic process-entry program which calls the supplied
// ExitProcess IAT entry with exit code 42. The caller adds the matching import.
std::vector<std::uint8_t> make_pe64_exit_process_stub();

// Calls NTDLL!NtYieldExecution, KERNEL32!GetCurrentProcessId and
// KERNEL32!RtlLookupFunctionEntry, then tail-jumps through KERNEL32!ExitProcess
// with code 42/43. Both tail-jump paths restore the entry RSP before transfer.
// It exercises two DLL descriptors, multiple name imports and FF 25 IAT jumps.
std::vector<std::uint8_t> make_pe64_multi_import_stub();

} // namespace xfrontend
