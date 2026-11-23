#pragma once

#include "xfrontend/linker.hpp"
#include "xfrontend/pe64_dll.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

// Numeric Win32 resource identifiers are emitted as a deterministic three-level
// type/name/language tree. Resource payload bytes are copied verbatim.
struct Pe64ResourceItem {
    std::uint16_t type_id = 0U;
    std::uint16_t name_id = 0U;
    std::uint16_t language_id = 0U;
    std::uint32_t code_page = 0U;
    std::vector<std::uint8_t> data;
};

struct Pe64LoadConfigSpec {
    bool enabled = false;
    bool has_security_cookie = false;
    // Offset of the 8-byte cookie pointer storage in initialized .data.
    std::size_t security_cookie_data_offset = 0U;
    // Load Config stores VAs of global function-pointer cells for the check and
    // dispatch helpers. Those cells live in initialized .data; their contents
    // are initialized to the corresponding .text helper addresses and receive
    // DIR64 relocations. GuardCFFunctionTable entries are .text RVAs.
    bool guard_cf_enabled = false;
    std::size_t guard_cf_check_pointer_data_offset = 0U;
    std::size_t guard_cf_check_target_text_offset = 0U;
    std::size_t guard_cf_dispatch_pointer_data_offset = 0U;
    std::size_t guard_cf_dispatch_target_text_offset = 0U;
    std::vector<std::size_t> guard_cf_function_offsets;
};

struct Pe64DelayImport {
    Pe64Import import;
    std::uint32_t timestamp = 0U;
};

struct Pe64BoundImport {
    std::string dll_name;
    std::uint32_t timestamp = 0U;
};

struct Pe64DllAdvancedSpec {
    std::vector<Pe64ResourceItem> resources;
    Pe64LoadConfigSpec load_config;
    std::vector<Pe64DelayImport> delay_imports;
    std::vector<Pe64BoundImport> bound_imports;
};

struct Pe64DllAdvancedInfo {
    std::uint32_t resource_directory_rva = 0U;
    std::uint32_t resource_directory_size = 0U;
    std::uint32_t load_config_directory_rva = 0U;
    std::uint32_t load_config_directory_size = 0U;
    std::uint32_t delay_import_directory_rva = 0U;
    std::uint32_t delay_import_directory_size = 0U;
    std::uint32_t bound_import_directory_rva = 0U;
    std::uint32_t bound_import_directory_size = 0U;
    std::uint32_t advanced_relocation_directory_rva = 0U;
    std::uint32_t advanced_relocation_directory_size = 0U;
    std::size_t resource_count = 0U;
    std::size_t delay_import_count = 0U;
    std::size_t bound_import_count = 0U;
    std::size_t guard_cf_function_count = 0U;
};

struct Pe64DllAdvancedResult {
    Pe64DllImage image;
    Pe64DllAdvancedInfo advanced;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept {
        return diagnostics.empty() && image.bytes.size() >= 0x200U;
    }
};

// Adds PE resource, load-configuration/GuardCF, delay-import and bound-import
// directories to an N38-compatible PE32+ DLL. The base writer remains the
// owner of code, exports, ordinary imports, TLS and AMD64 unwind metadata.
Pe64DllAdvancedResult write_pe64_dll_image_advanced(
    const Pe64DllSpec& base, const Pe64DllAdvancedSpec& advanced);

struct Pe64CoffExport {
    std::string symbol_name;
    std::uint32_t ordinal = 0U;
};

struct Pe64CoffDllLinkSpec {
    std::string dll_name;
    std::uint64_t image_base = 0x180000000ULL;
    // Supply either validated object models or raw AMD64 COFF .obj bytes.
    // Raw archives must be standard COFF LIB containers with COFF object members.
    std::vector<ObjectFile> input_objects;
    std::vector<StaticArchive> archives;
    std::vector<std::vector<std::uint8_t>> coff_object_files;
    std::vector<std::vector<std::uint8_t>> coff_library_files;
    std::vector<Pe64CoffExport> exports;
    std::vector<Pe64Import> imports;
    std::vector<Pe64DllRuntimeFunction> runtime_functions; // offsets in final linked .text
    std::string entry_symbol;
    Pe64DllAdvancedSpec advanced;
    std::size_t maximum_image_size = 256U * 1024U * 1024U;
};

struct Pe64CoffDllLinkResult {
    Pe64DllAdvancedResult output;
    LinkerResult link;
    std::vector<Diagnostic> diagnostics;
    std::size_t selected_object_count = 0U;
    std::size_t extracted_archive_member_count = 0U;
    std::size_t applied_relocation_count = 0U;
    bool succeeded() const noexcept {
        return diagnostics.empty() && output.succeeded() && link.succeeded();
    }
};

// Parses AMD64 COFF .obj / COFF LIB bytes (or accepts validated object models),
// selects archive members, resolves symbols and supported relocations, then
// emits a PE32+ DLL. OS imports are supplied explicitly in `imports`; arbitrary
// COFF import-thunk resolution and all Microsoft linker extensions are not implied.
Pe64CoffDllLinkResult link_coff_objects_to_pe64_dll(
    const Pe64CoffDllLinkSpec& spec);

} // namespace xfrontend
