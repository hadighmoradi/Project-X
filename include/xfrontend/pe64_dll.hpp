#pragma once

#include "xfrontend/diagnostic.hpp"
#include "xfrontend/pe64_image.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

// One exported name/ordinal binding. Multiple records may share an ordinal
// to represent aliases. An empty name creates an ordinal-only export.
// Forwarders store a DLL.symbol or DLL.#ordinal string in the EAT instead of
// an RVA to one of this image's sections.
struct Pe64DllExport {
    std::string name;
    std::uint32_t ordinal = 0U;
    Pe64SectionKind target_section = Pe64SectionKind::Text;
    std::size_t target_offset = 0U;
    bool forwarded = false;
    std::string forwarder;
};

struct Pe64DllTlsCallback {
    // TLS callbacks must point to executable code in the DLL .text section.
    Pe64SectionKind target_section = Pe64SectionKind::Text;
    std::size_t target_offset = 0U;
};

struct Pe64DllTlsSpec {
    bool enabled = false;
    // Initialized per-thread TLS template bytes. The loader appends zero-fill bytes.
    std::vector<std::uint8_t> template_data;
    std::uint32_t zero_fill_size = 0U;
    // Four-byte, zero-initialized, writable slot in the caller-provided .data section.
    std::size_t index_data_offset = 0U;
    std::vector<Pe64DllTlsCallback> callbacks;
};

enum class Pe64DllUnwindOperation : std::uint8_t {
    PushNonVol = 0U,       // UWOP_PUSH_NONVOL
    AllocLarge = 1U,       // UWOP_ALLOC_LARGE, OpInfo=0 (scaled 16-bit operand)
    AllocSmall = 2U,       // UWOP_ALLOC_SMALL
    SetFramePointer = 3U,  // UWOP_SET_FPREG
    SaveNonVol = 4U,       // UWOP_SAVE_NONVOL (scaled 16-bit operand)
    SaveNonVolFar = 5U,    // UWOP_SAVE_NONVOL_FAR (unscaled 32-bit operand)
    SaveXmm128 = 8U,       // UWOP_SAVE_XMM128 (scaled 16-bit operand)
    SaveXmm128Far = 9U,   // UWOP_SAVE_XMM128_FAR (unscaled 32-bit operand)
    PushMachineFrame = 10U // UWOP_PUSH_MACHFRAME; OpInfo selects error-code presence
};

struct Pe64DllUnwindCode {
    // Byte offset of the instruction end inside the function prologue.
    std::uint8_t code_offset = 0U;
    Pe64DllUnwindOperation operation = Pe64DllUnwindOperation::AllocSmall;
    std::uint8_t operation_info = 0U;
    // Operation-specific byte count, register number, or stack offset.
    std::uint32_t operand = 0U;
};

struct Pe64DllRuntimeFunction {
    std::size_t begin_offset = 0U;
    std::size_t end_offset = 0U; // exclusive .text offset
    std::uint8_t prolog_size = 0U;
    std::uint8_t frame_register = 0U;
    std::uint8_t frame_offset = 0U;
    // Input codes must be in descending CodeOffset order.
    std::vector<Pe64DllUnwindCode> unwind_codes;
    // UNW_FLAG_EHANDLER (1), UNW_FLAG_UHANDLER (2), or both (3).
    // UNW_FLAG_CHAININFO (4) instead references chained_function_index.
    std::uint8_t flags = 0U;
    std::size_t handler_offset = 0U; // .text offset, used when flags contains 1 or 2
    std::vector<std::uint8_t> handler_data; // opaque language-specific handler payload
    std::size_t chained_function_index = static_cast<std::size_t>(-1); // index in Pe64DllSpec::runtime_functions
};

struct Pe64DllSpec {
    std::string dll_name;
    std::uint64_t image_base = 0x180000000ULL;
    std::uint32_t section_alignment = 0x1000U;
    std::uint32_t file_alignment = 0x200U;
    std::size_t maximum_file_size = 512U * 1024U * 1024U;
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> read_only_data;
    std::vector<std::uint8_t> data;
    std::vector<Pe64AbsoluteRelocation> absolute_relocations;
    std::vector<Pe64DllExport> exports;
    std::vector<Pe64Import> imports;
    std::vector<Pe64IatCallSite> iat_call_sites;
    Pe64DllTlsSpec tls;
    // Optional AMD64 RUNTIME_FUNCTION / UNWIND_INFO records for code in .text.
    std::vector<Pe64DllRuntimeFunction> runtime_functions;
    // Optional PE AddressOfEntryPoint. For DLLs that require loader-mediated
    // thread notifications, point this at a minimal or application DllMain
    // in .text. The entrypoint must return TRUE for DLL_PROCESS_ATTACH.
    bool has_entry_point = false;
    std::size_t entry_point_offset = 0U;
};

struct Pe64DllSectionInfo {
    std::string name;
    std::uint32_t virtual_address = 0U;
    std::uint32_t virtual_size = 0U;
    std::uint32_t raw_offset = 0U;
    std::uint32_t raw_size = 0U;
    std::uint32_t characteristics = 0U;
};

struct Pe64DllImage {
    std::vector<std::uint8_t> bytes;
    std::vector<Pe64DllSectionInfo> sections;
    std::uint64_t image_base = 0U;
    std::uint32_t size_of_image = 0U;
    std::uint32_t size_of_headers = 0U;
    std::uint32_t entry_point_rva = 0U;
    std::uint32_t export_directory_rva = 0U;
    std::uint32_t export_directory_size = 0U;
    std::uint32_t relocation_directory_rva = 0U;
    std::uint32_t relocation_directory_size = 0U;
    std::size_t base_relocation_count = 0U;
    bool has_dynamic_base = false;
    std::size_t export_name_count = 0U;
    std::size_t export_ordinal_count = 0U;
    std::uint32_t import_directory_rva = 0U;
    std::uint32_t import_directory_size = 0U;
    std::uint32_t iat_rva = 0U;
    std::uint32_t iat_size = 0U;
    std::size_t imported_symbol_count = 0U;
    std::size_t imported_name_count = 0U;
    std::size_t imported_ordinal_count = 0U;
    std::size_t imported_library_count = 0U;
    std::uint32_t tls_directory_rva = 0U;
    std::uint32_t tls_directory_size = 0U;
    std::uint32_t tls_index_rva = 0U;
    std::uint32_t tls_template_rva = 0U;
    std::uint32_t tls_template_size = 0U;
    std::uint32_t tls_zero_fill_size = 0U;
    std::size_t tls_callback_count = 0U;
    std::uint32_t exception_directory_rva = 0U;
    std::uint32_t exception_directory_size = 0U;
    std::size_t runtime_function_count = 0U;
    std::size_t unwind_info_size = 0U;
};

struct Pe64DllResult {
    Pe64DllImage image;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept {
        return diagnostics.empty() && image.bytes.size() >= 0x200U;
    }
};

// Emits a PE32+ AMD64 DLL image containing .text, optional .rdata/.data, a
// generated .edata directory, optional .idata Import Directory/IAT, optional
// IMAGE_TLS_DIRECTORY64 metadata and callbacks, AMD64 .pdata/.xdata UNWIND_INFO
// v1 records including exception/termination handlers and chained records, and optional IMAGE_REL_BASED_DIR64 .reloc data. Imports support
// name/ordinal lookup and FF 15/FF 25 RIP-relative call/jump sites. TLS is
// loader-managed. An optional AddressOfEntryPoint may identify a caller-provided
// DllMain in .text; CRT startup and language-specific EH handlers remain outside
// this writer's scope.
Pe64DllResult write_pe64_dll_image(const Pe64DllSpec& spec);

} // namespace xfrontend
