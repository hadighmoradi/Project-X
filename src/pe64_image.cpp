// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/pe64_image.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace xfrontend {
namespace {

constexpr std::uint32_t kDosHeaderSize = 0x80U;
constexpr std::uint32_t kPeSignatureSize = 4U;
constexpr std::uint32_t kCoffHeaderSize = 20U;
constexpr std::uint32_t kOptionalHeaderSize = 240U;
constexpr std::uint32_t kSectionHeaderSize = 40U;
constexpr std::uint16_t kMachineAmd64 = 0x8664U;
constexpr std::uint16_t kPe32PlusMagic = 0x020bU;
constexpr std::uint16_t kExecutableImage = 0x0002U;
constexpr std::uint16_t kLargeAddressAware = 0x0020U;
constexpr std::uint16_t kDynamicBase = 0x0040U;
constexpr std::uint16_t kNxCompat = 0x0100U;
constexpr std::uint32_t kSectionCodeRx = 0x60000020U;
constexpr std::uint32_t kSectionReadOnly = 0x40000040U;
constexpr std::uint32_t kSectionReadWrite = 0xc0000040U;
constexpr std::uint32_t kSectionRelocRead = 0x42000040U;
constexpr std::uint16_t kRelocAbsolute = 0U;
constexpr std::uint16_t kRelocDir64 = 10U;
constexpr std::size_t kMaximumImports = 4096U;
constexpr std::size_t kMaximumRelocations = 65536U;
constexpr std::size_t kMaximumUnwindFunctions = 4096U;
constexpr std::uint64_t kOrdinalFlag64 = 0x8000000000000000ULL;

struct ImportLibrary {
    std::string canonical_name;
    std::string original_name;
    std::vector<std::size_t> import_indices;
};

struct SectionBuild {
    Pe64SectionInfo info;
    std::vector<std::uint8_t> raw;
};

struct RelocBlock {
    std::uint32_t page_rva = 0U;
    std::vector<std::uint16_t> entries;
};

std::string ascii_lower(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

bool has_nul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

bool is_power_of_two(std::uint32_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool checked_align(std::uint64_t value, std::uint32_t alignment, std::uint64_t& result) {
    if (!is_power_of_two(alignment)) return false;
    const std::uint64_t mask = static_cast<std::uint64_t>(alignment) - 1U;
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) return false;
    result = (value + mask) & ~mask;
    return true;
}

void put16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

void put64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

void append16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 2U);
    put16(bytes, offset, value);
}

void append32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 4U);
    put32(bytes, offset, value);
}

void append_string(std::vector<std::uint8_t>& bytes, const std::string& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0U);
}

bool fits_u32(std::uint64_t value) {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

bool fits_i32(std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

void diagnostic(Pe64ExecutableResult& result, const char* code, const std::string& message) {
    result.diagnostics.push_back(Diagnostic{code, message, SourceSpan{}});
}

bool section_target_rva(const Pe64ExecutableSpec& spec, Pe64SectionKind section,
                        std::size_t offset, std::uint32_t& result) {
    std::uint64_t base = 0U;
    std::uint64_t limit = 0U;
    switch (section) {
    case Pe64SectionKind::Text:
        base = 0x1000U; limit = spec.text.size(); break;
    case Pe64SectionKind::ReadOnlyData:
        base = 0x2000U; limit = spec.read_only_data.size(); break;
    case Pe64SectionKind::Data:
        base = 0x3000U; limit = spec.data.size() + spec.bss_size; break;
    default: return false;
    }
    if (static_cast<std::uint64_t>(offset) >= limit) return false;
    const std::uint64_t value = base + static_cast<std::uint64_t>(offset);
    if (!fits_u32(value)) return false;
    result = static_cast<std::uint32_t>(value);
    return true;
}

std::vector<std::uint8_t> build_import_section(const Pe64ExecutableSpec& spec,
                                               std::uint32_t rdata_rva,
                                               Pe64ExecutableImage& image,
                                               std::vector<std::uint32_t>& iat_rvas,
                                               Pe64ExecutableResult& result) {
    iat_rvas.assign(spec.imports.size(), 0U);
    image.imported_library_count = 0U;
    image.imported_symbol_count = 0U;
    image.imported_name_count = 0U;
    image.imported_ordinal_count = 0U;
    if (spec.imports.empty()) return {};

    std::vector<ImportLibrary> libraries;
    std::map<std::string, std::size_t> library_map;
    for (std::size_t i = 0U; i < spec.imports.size(); ++i) {
        const auto& item = spec.imports[i];
        const std::string key = ascii_lower(item.dll_name);
        auto found = library_map.find(key);
        if (found == library_map.end()) {
            const std::size_t index = libraries.size();
            library_map.emplace(key, index);
            libraries.push_back(ImportLibrary{key, item.dll_name, {i}});
        } else {
            libraries[found->second].import_indices.push_back(i);
        }
    }

    std::vector<std::uint8_t> bytes;
    const std::size_t descriptor_size = (libraries.size() + 1U) * 20U;
    const std::uint32_t ilt_base = static_cast<std::uint32_t>((descriptor_size + 7U) & ~std::size_t{7U});
    std::size_t ilt_total = 0U;
    for (const auto& library : libraries) ilt_total += (library.import_indices.size() + 1U) * 8U;
    const std::uint32_t iat_base = ilt_base + static_cast<std::uint32_t>(ilt_total);
    const std::size_t iat_total = ilt_total;
    bytes.resize(static_cast<std::size_t>(iat_base) + iat_total, 0U);

    std::size_t ilt_cursor = ilt_base;
    std::size_t iat_cursor = iat_base;
    std::vector<std::size_t> descriptor_offsets;
    for (std::size_t library_index = 0U; library_index < libraries.size(); ++library_index) {
        const auto& library = libraries[library_index];
        const std::size_t descriptor = library_index * 20U;
        descriptor_offsets.push_back(descriptor);
        put32(bytes, descriptor, rdata_rva + static_cast<std::uint32_t>(ilt_cursor));
        const std::uint32_t name_offset = static_cast<std::uint32_t>(bytes.size());
        (void)name_offset;
        // The name is appended after all thunk arrays; descriptor RVA is patched below.
        put32(bytes, descriptor + 12U, 0U);
        put32(bytes, descriptor + 16U, rdata_rva + static_cast<std::uint32_t>(iat_cursor));

        for (std::size_t import_index : library.import_indices) {
            const auto& imported = spec.imports[import_index];
            std::uint64_t thunk_value = 0U;
            if (imported.kind == Pe64ImportKind::ByOrdinal) {
                thunk_value = kOrdinalFlag64 | static_cast<std::uint64_t>(imported.ordinal);
                ++image.imported_ordinal_count;
            } else {
                ++image.imported_name_count;
                const std::size_t name_pos = (bytes.size() + 1U) & ~std::size_t{1U};
                if (bytes.size() < name_pos) bytes.resize(name_pos, 0U);
                const std::uint32_t ibn_rva = rdata_rva + static_cast<std::uint32_t>(name_pos);
                append16(bytes, imported.hint);
                append_string(bytes, imported.symbol_name);
                thunk_value = ibn_rva;
            }
            put64(bytes, ilt_cursor, thunk_value);
            put64(bytes, iat_cursor, thunk_value);
            iat_rvas[import_index] = rdata_rva + static_cast<std::uint32_t>(iat_cursor);
            ilt_cursor += 8U;
            iat_cursor += 8U;
        }
        // Reserved zero terminators are already present in each thunk table.
        ilt_cursor += 8U;
        iat_cursor += 8U;
    }

    // DLL names are appended last so no descriptor/string overlap is possible.
    for (std::size_t library_index = 0U; library_index < libraries.size(); ++library_index) {
        const std::size_t name_pos = bytes.size();
        append_string(bytes, libraries[library_index].original_name);
        put32(bytes, descriptor_offsets[library_index] + 12U,
              rdata_rva + static_cast<std::uint32_t>(name_pos));
    }

    if (!libraries.empty()) {
        image.import_directory_rva = rdata_rva;
        image.import_directory_size = static_cast<std::uint32_t>(descriptor_size);
        image.iat_rva = rdata_rva + iat_base;
        image.iat_size = static_cast<std::uint32_t>(iat_total);
    }
    image.imported_library_count = libraries.size();
    image.imported_symbol_count = spec.imports.size();
    (void)result;
    return bytes;
}

std::vector<std::uint8_t> build_relocation_section(const Pe64ExecutableSpec& spec,
                                                    std::uint32_t relocation_rva,
                                                    Pe64ExecutableImage& image,
                                                    Pe64ExecutableResult& result) {
    std::map<std::uint32_t, std::vector<std::uint16_t>> grouped;
    for (const auto& item : spec.absolute_relocations) {
        const std::uint32_t destination_rva = 0x3000U + static_cast<std::uint32_t>(item.destination_offset);
        const std::uint32_t page = destination_rva & ~0xfffU;
        const std::uint16_t offset = static_cast<std::uint16_t>(destination_rva & 0x0fffU);
        grouped[page].push_back(static_cast<std::uint16_t>((kRelocDir64 << 12U) | offset));
    }

    std::vector<std::uint8_t> bytes;
    for (auto& group : grouped) {
        auto& entries = group.second;
        std::sort(entries.begin(), entries.end());
        append32(bytes, group.first);
        const std::size_t block_size_offset = bytes.size();
        append32(bytes, 0U);
        for (std::uint16_t entry : entries) append16(bytes, entry);
        if ((entries.size() & 1U) != 0U) append16(bytes, static_cast<std::uint16_t>(kRelocAbsolute << 12U));
        put32(bytes, block_size_offset, static_cast<std::uint32_t>(bytes.size() - (block_size_offset - 4U)));
    }
    image.base_relocation_count = spec.absolute_relocations.size();
    if (!bytes.empty()) {
        image.has_dynamic_base = true;
        image.relocation_directory_rva = relocation_rva;
        image.relocation_directory_size = static_cast<std::uint32_t>(bytes.size());
    }
    (void)result;
    return bytes;
}

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> build_unwind_sections(
    const Pe64ExecutableSpec& spec, std::uint32_t pdata_rva, std::uint32_t xdata_rva,
    Pe64ExecutableImage& image) {
    std::vector<const Pe64FunctionUnwind*> functions;
    functions.reserve(spec.function_unwind.size());
    for (const auto& function : spec.function_unwind) functions.push_back(&function);
    std::sort(functions.begin(), functions.end(), [](const Pe64FunctionUnwind* left,
                                                     const Pe64FunctionUnwind* right) {
        return left->begin_offset < right->begin_offset;
    });

    std::vector<std::uint8_t> pdata;
    std::vector<std::uint8_t> xdata;
    for (const Pe64FunctionUnwind* function : functions) {
        while ((xdata.size() & 3U) != 0U) xdata.push_back(0U);
        const std::uint32_t unwind_info_rva = xdata_rva + static_cast<std::uint32_t>(xdata.size());
        xdata.push_back(1U); // Version 1, no flags.
        xdata.push_back(function->prolog_size);
        xdata.push_back(static_cast<std::uint8_t>(function->codes.size()));
        xdata.push_back(0U); // No frame register or frame offset.

        std::vector<Pe64UnwindCode> codes = function->codes;
        std::sort(codes.begin(), codes.end(), [](const Pe64UnwindCode& left, const Pe64UnwindCode& right) {
            return left.code_offset > right.code_offset;
        });
        for (const auto& code : codes) {
            std::uint8_t op_info = 0U;
            if (code.operation == Pe64UnwindOperation::AllocSmall) {
                op_info = static_cast<std::uint8_t>((code.operand - 8U) / 8U);
            } else if (code.operation == Pe64UnwindOperation::PushNonVol) {
                op_info = static_cast<std::uint8_t>(code.operand);
            }
            xdata.push_back(code.code_offset);
            xdata.push_back(static_cast<std::uint8_t>((op_info << 4U) |
                static_cast<std::uint8_t>(code.operation)));
        }
        if ((function->codes.size() & 1U) != 0U) {
            xdata.push_back(0U);
            xdata.push_back(0U);
        }
        while ((xdata.size() & 3U) != 0U) xdata.push_back(0U);

        append32(pdata, 0x1000U + static_cast<std::uint32_t>(function->begin_offset));
        append32(pdata, 0x1000U + static_cast<std::uint32_t>(function->end_offset));
        append32(pdata, unwind_info_rva);
    }
    if (!pdata.empty()) {
        image.exception_directory_rva = pdata_rva;
        image.exception_directory_size = static_cast<std::uint32_t>(pdata.size());
        image.pdata_rva = pdata_rva;
        image.xdata_rva = xdata_rva;
        image.unwind_function_count = functions.size();
    }
    return {std::move(pdata), std::move(xdata)};
}

} // namespace

std::vector<std::uint8_t> make_pe64_exit_process_stub() {
    // sub rsp, 28h; mov ecx, 42; call qword ptr [rip+disp32]; ud2
    return {0x48U, 0x83U, 0xecU, 0x28U,
            0xb9U, 0x2aU, 0x00U, 0x00U, 0x00U,
            0xffU, 0x15U, 0x00U, 0x00U, 0x00U, 0x00U,
            0x0fU, 0x0bU};
}

std::vector<std::uint8_t> make_pe64_multi_import_stub() {
    // sub rsp, 28h
    // call [rip+NtYieldExecution]; call [rip+GetCurrentProcessId]
    // lea rcx,[rip+0] (a control PC inside this function)
    // lea rdx,[rsp+20h]; xor r8d,r8d; call [rip+RtlLookupFunctionEntry]
    // test rax,rax; jz failure; mov ecx,42; add rsp,28h; jmp [rip+ExitProcess]
    // failure: mov ecx,43; add rsp,28h; jmp [rip+ExitProcess]; ud2
    // Both indirect tail jumps restore the entry stack before transferring control,
    // preserving the Windows x64 entry alignment and the caller's return address.
    return {0x48U, 0x83U, 0xecU, 0x28U,
            0xffU, 0x15U, 0x00U, 0x00U, 0x00U, 0x00U,
            0xffU, 0x15U, 0x00U, 0x00U, 0x00U, 0x00U,
            0x48U, 0x8dU, 0x0dU, 0x00U, 0x00U, 0x00U, 0x00U,
            0x48U, 0x8dU, 0x54U, 0x24U, 0x20U,
            0x45U, 0x31U, 0xc0U,
            0xffU, 0x15U, 0x00U, 0x00U, 0x00U, 0x00U,
            0x48U, 0x85U, 0xc0U,
            0x74U, 0x0fU,
            0xb9U, 0x2aU, 0x00U, 0x00U, 0x00U,
            0x48U, 0x83U, 0xc4U, 0x28U,
            0xffU, 0x25U, 0x00U, 0x00U, 0x00U, 0x00U,
            0xb9U, 0x2bU, 0x00U, 0x00U, 0x00U,
            0x48U, 0x83U, 0xc4U, 0x28U,
            0xffU, 0x25U, 0x00U, 0x00U, 0x00U, 0x00U,
            0x0fU, 0x0bU};
}

Pe64ExecutableResult write_pe64_executable_image(const Pe64ExecutableSpec& spec) {
    Pe64ExecutableResult result;
    if (spec.text.empty()) {
        diagnostic(result, "XPE0001", "PE32+ executable requires a non-empty .text section");
        return result;
    }
    if (spec.entry_point_offset >= spec.text.size()) {
        diagnostic(result, "XPE0002", "entry point offset is outside .text");
        return result;
    }
    if (spec.image_base == 0U || (spec.image_base & 0xffffU) != 0U) {
        diagnostic(result, "XPE0003", "image base must be non-zero and 64 KiB aligned");
        return result;
    }
    if (!is_power_of_two(spec.section_alignment) || !is_power_of_two(spec.file_alignment) ||
        spec.file_alignment < 0x200U || spec.file_alignment > 0x10000U ||
        spec.section_alignment != 0x1000U || spec.section_alignment < spec.file_alignment) {
        diagnostic(result, "XPE0004", "invalid PE section/file alignment");
        return result;
    }
    if (spec.subsystem != 2U && spec.subsystem != 3U) {
        diagnostic(result, "XPE0005", "N30 supports only Windows GUI or console subsystem values");
        return result;
    }
    if (spec.maximum_file_size < 0x400U) {
        diagnostic(result, "XPE0006", "maximum file size is smaller than the PE headers");
        return result;
    }
    if (spec.imports.size() > kMaximumImports || spec.absolute_relocations.size() > kMaximumRelocations) {
        diagnostic(result, "XPE0007", "import or base-relocation count exceeds the N30 safety limit");
        return result;
    }

    using ImportKey = std::tuple<std::string, std::uint8_t, std::string, std::uint16_t>;
    std::set<ImportKey> import_keys;
    for (const auto& item : spec.imports) {
        if (item.dll_name.empty() || has_nul(item.dll_name)) {
            diagnostic(result, "XPE0008", "import DLL name must be non-empty and NUL-free");
            return result;
        }
        if (item.dll_name.size() > 260U) {
            diagnostic(result, "XPE0025", "import DLL name exceeds the bounded string limit");
            return result;
        }
        if (item.kind == Pe64ImportKind::ByName) {
            if (item.symbol_name.empty() || has_nul(item.symbol_name)) {
                diagnostic(result, "XPE0008", "import-by-name symbol must be non-empty and NUL-free");
                return result;
            }
            if (item.symbol_name.size() > 4096U) {
                diagnostic(result, "XPE0025", "import symbol name exceeds the bounded string limit");
                return result;
            }
        } else if (item.kind == Pe64ImportKind::ByOrdinal) {
            if (!item.symbol_name.empty() || item.hint != 0U) {
                diagnostic(result, "XPE0027", "import-by-ordinal must not carry a symbol name or hint");
                return result;
            }
        } else {
            diagnostic(result, "XPE0026", "unknown PE import kind");
            return result;
        }
        const auto key = ImportKey{ascii_lower(item.dll_name), static_cast<std::uint8_t>(item.kind),
                                   item.symbol_name, item.ordinal};
        if (!import_keys.insert(key).second) {
            diagnostic(result, "XPE0009", "duplicate DLL/symbol-or-ordinal import is not permitted");
            return result;
        }
    }

    const auto find_import = [&spec](const Pe64IatCallSite& call) {
        return std::find_if(spec.imports.begin(), spec.imports.end(), [&call](const Pe64Import& item) {
            if (ascii_lower(item.dll_name) != ascii_lower(call.dll_name) || item.kind != call.import_kind)
                return false;
            if (item.kind == Pe64ImportKind::ByOrdinal) return item.ordinal == call.ordinal;
            return item.symbol_name == call.symbol_name;
        });
    };

    std::vector<std::pair<std::size_t, std::size_t>> call_site_ranges;
    for (const auto& call : spec.iat_call_sites) {
        if (call.instruction_kind != Pe64IatInstructionKind::CallIndirect &&
            call.instruction_kind != Pe64IatInstructionKind::JumpIndirect) {
            diagnostic(result, "XPE0028", "unknown IAT instruction kind");
            return result;
        }
        if (call.import_kind != Pe64ImportKind::ByName && call.import_kind != Pe64ImportKind::ByOrdinal) {
            diagnostic(result, "XPE0028", "unknown IAT import reference kind");
            return result;
        }
        const std::uint8_t expected_modrm = call.instruction_kind == Pe64IatInstructionKind::CallIndirect ? 0x15U : 0x25U;
        if (call.displacement_offset < 2U || call.displacement_offset > spec.text.size() ||
            spec.text.size() - call.displacement_offset < 4U ||
            spec.text[call.displacement_offset - 2U] != 0xffU ||
            spec.text[call.displacement_offset - 1U] != expected_modrm) {
            diagnostic(result, "XPE0010", "IAT site must reference the disp32 field of the selected FF 15/FF 25 instruction");
            return result;
        }
        if (call.import_kind == Pe64ImportKind::ByOrdinal && !call.symbol_name.empty()) {
            diagnostic(result, "XPE0027", "ordinal IAT reference must not carry a symbol name");
            return result;
        }
        const auto found = find_import(call);
        if (found == spec.imports.end()) {
            diagnostic(result, "XPE0011", "IAT call/jump site references an import that is not declared");
            return result;
        }
        call_site_ranges.emplace_back(call.displacement_offset - 2U, call.displacement_offset + 4U);
    }
    std::sort(call_site_ranges.begin(), call_site_ranges.end());
    for (std::size_t i = 1U; i < call_site_ranges.size(); ++i) {
        if (call_site_ranges[i].first < call_site_ranges[i - 1U].second) {
            diagnostic(result, "XPE0024", "overlapping IAT call-site instructions are not permitted");
            return result;
        }
    }

    std::set<std::size_t> relocation_destinations;
    for (const auto& relocation : spec.absolute_relocations) {
        if ((relocation.destination_offset & 7U) != 0U ||
            relocation.destination_offset > spec.data.size() ||
            spec.data.size() - relocation.destination_offset < 8U) {
            diagnostic(result, "XPE0012", "DIR64 relocation destination must be an aligned initialized .data qword");
            return result;
        }
        if (!relocation_destinations.insert(relocation.destination_offset).second) {
            diagnostic(result, "XPE0013", "overlapping or duplicate DIR64 relocation destinations are not permitted");
            return result;
        }
        std::uint32_t target_rva = 0U;
        if (!section_target_rva(spec, relocation.target_section, relocation.target_offset, target_rva)) {
            diagnostic(result, "XPE0014", "DIR64 relocation target is outside a supported image section");
            return result;
        }
        if (relocation.target_section == Pe64SectionKind::Data && relocation.target_offset >= spec.data.size() + spec.bss_size) {
            diagnostic(result, "XPE0014", "DIR64 relocation target is outside .data/.bss");
            return result;
        }
    }

    if (spec.function_unwind.size() > kMaximumUnwindFunctions) {
        diagnostic(result, "XPE0030", "function unwind record count exceeds the N30 safety limit");
        return result;
    }
    std::vector<std::pair<std::size_t, std::size_t>> unwind_ranges;
    for (const auto& function : spec.function_unwind) {
        if (function.begin_offset >= function.end_offset || function.end_offset > spec.text.size() ||
            function.prolog_size > function.end_offset - function.begin_offset) {
            diagnostic(result, "XPE0031", "unwind function range or prologue size is outside .text");
            return result;
        }
        if (function.codes.size() > 255U) {
            diagnostic(result, "XPE0032", "UNWIND_INFO code slot count exceeds one-byte CountOfCodes");
            return result;
        }
        std::set<std::uint8_t> unwind_code_offsets;
        for (const auto& code : function.codes) {
            if (code.code_offset == 0U || code.code_offset > function.prolog_size ||
                !unwind_code_offsets.insert(code.code_offset).second) {
                diagnostic(result, "XPE0033", "unwind CodeOffset must be unique and inside the function prologue");
                return result;
            }
            if (code.operation == Pe64UnwindOperation::AllocSmall) {
                if (code.operand < 8U || code.operand > 128U || (code.operand & 7U) != 0U) {
                    diagnostic(result, "XPE0034", "UWOP_ALLOC_SMALL size must be 8..128 bytes in 8-byte units");
                    return result;
                }
            } else if (code.operation == Pe64UnwindOperation::PushNonVol) {
                const bool valid_register = code.operand == 3U || code.operand == 5U || code.operand == 6U ||
                    code.operand == 7U || (code.operand >= 12U && code.operand <= 15U);
                if (!valid_register) {
                    diagnostic(result, "XPE0034", "UWOP_PUSH_NONVOL requires a nonvolatile AMD64 GPR register id");
                    return result;
                }
            } else {
                diagnostic(result, "XPE0035", "unsupported AMD64 UNWIND_CODE operation in N30");
                return result;
            }
        }
        unwind_ranges.emplace_back(function.begin_offset, function.end_offset);
    }
    std::sort(unwind_ranges.begin(), unwind_ranges.end());
    for (std::size_t index = 1U; index < unwind_ranges.size(); ++index) {
        if (unwind_ranges[index].first < unwind_ranges[index - 1U].second) {
            diagnostic(result, "XPE0036", "RUNTIME_FUNCTION ranges must not overlap");
            return result;
        }
    }

    // Fixed N30 layout: .text=1000, .rdata=2000, .data=3000,
    // .idata=4000 when imports exist, and .reloc=4000 or 5000 as needed.
    // Optional .pdata/.xdata follow relocation data at 4 KiB boundaries.
    if (spec.bss_size > std::numeric_limits<std::size_t>::max() - spec.data.size()) {
        diagnostic(result, "XPE0015", "combined .data/.bss virtual size overflows the host size type");
        return result;
    }
    const std::size_t data_virtual_size = spec.data.size() + spec.bss_size;
    if (spec.text.size() > 0x1000U || spec.read_only_data.size() > 0x1000U ||
        data_virtual_size > 0x1000U) {
        diagnostic(result, "XPE0015", "N30 fixed-layout section payload exceeds its 4 KiB RVA window");
        return result;
    }

    result.image.image_base = spec.image_base;
    result.image.entry_point_rva = 0x1000U + static_cast<std::uint32_t>(spec.entry_point_offset);

    std::vector<std::uint32_t> iat_rvas;
    std::vector<std::uint8_t> idata = build_import_section(spec, 0x4000U, result.image, iat_rvas, result);
    if (idata.size() > 0x1000U) {
        diagnostic(result, "XPE0025", ".idata Import Directory, thunks and names exceed its 4 KiB RVA window");
        result.image = Pe64ExecutableImage{};
        return result;
    }
    std::vector<std::uint8_t> rdata = spec.read_only_data;
    if (rdata.size() > 0x1000U) {
        diagnostic(result, "XPE0016", ".rdata payload exceeds the N30 section window");
        result.image = Pe64ExecutableImage{};
        return result;
    }
    if (rdata.empty()) rdata.push_back(0U);
    result.image.read_only_data_rva = 0x2000U;

    // Patch each FF 15 disp32 to the corresponding Import Address Table cell.
    for (const auto& call : spec.iat_call_sites) {
        const auto import_it = find_import(call);
        const std::size_t import_index = static_cast<std::size_t>(import_it - spec.imports.begin());
        const std::uint32_t iat_rva = iat_rvas[import_index];
        const std::int64_t instruction_end = static_cast<std::int64_t>(0x1000U) +
            static_cast<std::int64_t>(call.displacement_offset) + 4;
        const std::int64_t displacement = static_cast<std::int64_t>(iat_rva) - instruction_end;
        if (!fits_i32(displacement)) {
            diagnostic(result, "XPE0017", "RIP-relative IAT call displacement exceeds signed 32-bit range");
            result.image = Pe64ExecutableImage{};
            return result;
        }
        const std::uint32_t raw = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
        (void)raw;
    }

    std::vector<std::uint8_t> text = spec.text;
    for (const auto& call : spec.iat_call_sites) {
        const auto import_it = find_import(call);
        const std::size_t import_index = static_cast<std::size_t>(import_it - spec.imports.begin());
        const std::uint32_t iat_rva = iat_rvas[import_index];
        const std::int64_t instruction_end = static_cast<std::int64_t>(0x1000U) +
            static_cast<std::int64_t>(call.displacement_offset) + 4;
        const std::int32_t displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(iat_rva) - instruction_end);
        put32(text, call.displacement_offset, static_cast<std::uint32_t>(displacement));
    }
    std::vector<std::uint8_t> data = spec.data;
    for (const auto& relocation : spec.absolute_relocations) {
        std::uint32_t target_rva = 0U;
        (void)section_target_rva(spec, relocation.target_section, relocation.target_offset, target_rva);
        if (spec.image_base > std::numeric_limits<std::uint64_t>::max() - target_rva) {
            diagnostic(result, "XPE0018", "DIR64 relocation pointer value overflows 64-bit address space");
            result.image = Pe64ExecutableImage{};
            return result;
        }
        const std::uint64_t base_plus_rva = spec.image_base + static_cast<std::uint64_t>(target_rva);
        std::uint64_t pointer_value = 0U;
        if (relocation.addend >= 0) {
            const std::uint64_t addend = static_cast<std::uint64_t>(relocation.addend);
            if (base_plus_rva > std::numeric_limits<std::uint64_t>::max() - addend) {
                diagnostic(result, "XPE0018", "DIR64 relocation pointer value overflows 64-bit address space");
                result.image = Pe64ExecutableImage{};
                return result;
            }
            pointer_value = base_plus_rva + addend;
        } else {
            const std::uint64_t sub = static_cast<std::uint64_t>(-(relocation.addend + 1)) + 1U;
            if (base_plus_rva < sub) {
                diagnostic(result, "XPE0018", "DIR64 relocation pointer value underflows 64-bit address space");
                result.image = Pe64ExecutableImage{};
                return result;
            }
            pointer_value = base_plus_rva - sub;
        }
        put64(data, relocation.destination_offset, pointer_value);
    }
    const std::uint32_t relocation_rva = spec.imports.empty() ? 0x4000U : 0x5000U;
    std::vector<std::uint8_t> reloc = build_relocation_section(spec, relocation_rva, result.image, result);
    const std::uint32_t unwind_pdata_rva = relocation_rva + (reloc.empty() ? 0U : 0x1000U);
    const std::uint32_t unwind_xdata_rva = unwind_pdata_rva + 0x1000U;
    auto unwind_sections = build_unwind_sections(spec, unwind_pdata_rva, unwind_xdata_rva, result.image);
    std::vector<std::uint8_t> pdata = std::move(unwind_sections.first);
    std::vector<std::uint8_t> xdata = std::move(unwind_sections.second);
    if (pdata.size() > 0x1000U || xdata.size() > 0x1000U) {
        diagnostic(result, "XPE0037", ".pdata or .xdata exceeds its reserved 4 KiB RVA window");
        result.image = Pe64ExecutableImage{};
        return result;
    }

    const std::uint32_t section_count = 3U + (spec.imports.empty() ? 0U : 1U) +
        (reloc.empty() ? 0U : 1U) + (pdata.empty() ? 0U : 2U);
    const std::uint64_t headers_end = kDosHeaderSize + kPeSignatureSize + kCoffHeaderSize +
        kOptionalHeaderSize + static_cast<std::uint64_t>(section_count) * kSectionHeaderSize;
    std::uint64_t size_headers64 = 0U;
    if (!checked_align(headers_end, spec.file_alignment, size_headers64) || !fits_u32(size_headers64)) {
        diagnostic(result, "XPE0019", "PE header size overflows or cannot be aligned");
        result.image = Pe64ExecutableImage{};
        return result;
    }
    const std::uint32_t size_headers = static_cast<std::uint32_t>(size_headers64);

    std::vector<SectionBuild> sections;
    sections.push_back(SectionBuild{Pe64SectionInfo{".text", 0x1000U, static_cast<std::uint32_t>(text.size()), 0U, 0U, kSectionCodeRx}, std::move(text)});
    sections.push_back(SectionBuild{Pe64SectionInfo{".rdata", 0x2000U, static_cast<std::uint32_t>(rdata.size()), 0U, 0U, kSectionReadOnly}, std::move(rdata)});
    sections.push_back(SectionBuild{Pe64SectionInfo{".data", 0x3000U, static_cast<std::uint32_t>(spec.data.size() + spec.bss_size), 0U, 0U, kSectionReadWrite}, std::move(data)});
    if (!idata.empty()) sections.push_back(SectionBuild{Pe64SectionInfo{".idata", 0x4000U, static_cast<std::uint32_t>(idata.size()), 0U, 0U, kSectionReadWrite}, std::move(idata)});
    if (!reloc.empty()) sections.push_back(SectionBuild{Pe64SectionInfo{".reloc", relocation_rva, static_cast<std::uint32_t>(reloc.size()), 0U, 0U, kSectionRelocRead}, std::move(reloc)});
    if (!pdata.empty()) {
        sections.push_back(SectionBuild{Pe64SectionInfo{".pdata", unwind_pdata_rva,
            static_cast<std::uint32_t>(pdata.size()), 0U, 0U, kSectionReadOnly}, std::move(pdata)});
        sections.push_back(SectionBuild{Pe64SectionInfo{".xdata", unwind_xdata_rva,
            static_cast<std::uint32_t>(xdata.size()), 0U, 0U, kSectionReadOnly}, std::move(xdata)});
    }

    std::uint64_t raw_cursor = size_headers;
    for (auto& section : sections) {
        if (section.raw.empty()) {
            section.info.raw_offset = 0U;
            section.info.raw_size = 0U;
            continue;
        }
        if (!checked_align(raw_cursor, spec.file_alignment, raw_cursor) || !fits_u32(raw_cursor)) {
            diagnostic(result, "XPE0020", "section raw file offset overflows PE32+");
            result.image = Pe64ExecutableImage{};
            return result;
        }
        section.info.raw_offset = static_cast<std::uint32_t>(raw_cursor);
        std::uint64_t raw_size = 0U;
        if (!checked_align(section.raw.size(), spec.file_alignment, raw_size) || !fits_u32(raw_size)) {
            diagnostic(result, "XPE0021", "section raw size overflows PE32+");
            result.image = Pe64ExecutableImage{};
            return result;
        }
        section.info.raw_size = static_cast<std::uint32_t>(raw_size);
        raw_cursor += raw_size;
        if (!fits_u32(raw_cursor) || raw_cursor > spec.maximum_file_size) {
            diagnostic(result, "XPE0022", "PE image exceeds maximum file size");
            result.image = Pe64ExecutableImage{};
            return result;
        }
    }

    const auto last_end = static_cast<std::uint64_t>(sections.back().info.virtual_address) +
        std::max(sections.back().info.virtual_size, sections.back().info.raw_size);
    std::uint64_t size_image64 = 0U;
    if (!checked_align(last_end, spec.section_alignment, size_image64) || !fits_u32(size_image64)) {
        diagnostic(result, "XPE0023", "SizeOfImage overflows PE32+");
        result.image = Pe64ExecutableImage{};
        return result;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(raw_cursor), 0U);
    // DOS header and compatibility stub.
    bytes[0] = 'M'; bytes[1] = 'Z';
    put32(bytes, 0x3cU, kDosHeaderSize);
    const std::string dos_message = "This program cannot be run in DOS mode.\r\n$";
    std::copy(dos_message.begin(), dos_message.end(), bytes.begin() + 0x40U);

    const std::size_t pe_offset = kDosHeaderSize;
    bytes[pe_offset] = 'P'; bytes[pe_offset + 1U] = 'E'; bytes[pe_offset + 2U] = 0U; bytes[pe_offset + 3U] = 0U;
    const std::size_t coff = pe_offset + 4U;
    put16(bytes, coff, kMachineAmd64);
    put16(bytes, coff + 2U, static_cast<std::uint16_t>(section_count));
    put32(bytes, coff + 4U, 0U); // reproducible timestamp
    put32(bytes, coff + 8U, 0U); // symbol table pointer
    put32(bytes, coff + 12U, 0U); // symbol count
    put16(bytes, coff + 16U, static_cast<std::uint16_t>(kOptionalHeaderSize));
    put16(bytes, coff + 18U, static_cast<std::uint16_t>(kExecutableImage | kLargeAddressAware));

    const std::size_t opt = coff + kCoffHeaderSize;
    put16(bytes, opt, kPe32PlusMagic);
    bytes[opt + 2U] = 14U; // linker major
    bytes[opt + 3U] = 0U;
    put32(bytes, opt + 4U, sections[0].info.raw_size);
    std::uint64_t initialized = 0U;
    for (std::size_t i = 1U; i < sections.size(); ++i) initialized += sections[i].info.raw_size;
    put32(bytes, opt + 8U, static_cast<std::uint32_t>(initialized));
    put32(bytes, opt + 12U, static_cast<std::uint32_t>(spec.bss_size));
    put32(bytes, opt + 16U, result.image.entry_point_rva);
    put32(bytes, opt + 20U, sections[0].info.virtual_address);
    put64(bytes, opt + 24U, spec.image_base);
    put32(bytes, opt + 32U, spec.section_alignment);
    put32(bytes, opt + 36U, spec.file_alignment);
    put16(bytes, opt + 40U, 6U); // OS major
    put16(bytes, opt + 42U, 0U);
    put16(bytes, opt + 44U, 1U); // image major
    put16(bytes, opt + 46U, 0U);
    put16(bytes, opt + 48U, 6U); // subsystem major
    put16(bytes, opt + 50U, 0U);
    put32(bytes, opt + 52U, 0U);
    put32(bytes, opt + 56U, static_cast<std::uint32_t>(size_image64));
    put32(bytes, opt + 60U, size_headers);
    put32(bytes, opt + 64U, 0U); // checksum is left for the OS/tooling
    put16(bytes, opt + 68U, spec.subsystem);
    std::uint16_t dll_characteristics = kNxCompat;
    if (!spec.absolute_relocations.empty()) dll_characteristics = static_cast<std::uint16_t>(dll_characteristics | kDynamicBase);
    put16(bytes, opt + 70U, dll_characteristics);
    put64(bytes, opt + 72U, 0x100000U); // stack reserve
    put64(bytes, opt + 80U, 0x1000U);   // stack commit
    put64(bytes, opt + 88U, 0x100000U); // heap reserve
    put64(bytes, opt + 96U, 0x1000U);   // heap commit
    put32(bytes, opt + 104U, 0U);
    put32(bytes, opt + 108U, 16U);
    const std::size_t dirs = opt + 112U;
    // Export directory (0) remains absent.
    if (result.image.import_directory_rva != 0U) {
        put32(bytes, dirs + 8U, result.image.import_directory_rva);
        put32(bytes, dirs + 12U, result.image.import_directory_size);
    }
    if (result.image.exception_directory_rva != 0U) {
        put32(bytes, dirs + 3U * 8U, result.image.exception_directory_rva);
        put32(bytes, dirs + 3U * 8U + 4U, result.image.exception_directory_size);
    }
    if (result.image.relocation_directory_rva != 0U) {
        put32(bytes, dirs + 5U * 8U, result.image.relocation_directory_rva);
        put32(bytes, dirs + 5U * 8U + 4U, result.image.relocation_directory_size);
    }
    if (result.image.iat_rva != 0U) {
        put32(bytes, dirs + 12U * 8U, result.image.iat_rva);
        put32(bytes, dirs + 12U * 8U + 4U, result.image.iat_size);
    }

    const std::size_t section_headers = opt + kOptionalHeaderSize;
    for (std::size_t i = 0U; i < sections.size(); ++i) {
        const auto& section = sections[i];
        const std::size_t header = section_headers + i * kSectionHeaderSize;
        std::copy(section.info.name.begin(), section.info.name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(header));
        put32(bytes, header + 8U, section.info.virtual_size);
        put32(bytes, header + 12U, section.info.virtual_address);
        put32(bytes, header + 16U, section.info.raw_size);
        put32(bytes, header + 20U, section.info.raw_offset);
        put32(bytes, header + 24U, 0U);
        put32(bytes, header + 28U, 0U);
        put16(bytes, header + 32U, 0U);
        put16(bytes, header + 34U, 0U);
        put32(bytes, header + 36U, section.info.characteristics);
        if (!section.raw.empty())
            std::copy(section.raw.begin(), section.raw.end(), bytes.begin() + section.info.raw_offset);
        result.image.sections.push_back(section.info);
    }

    result.image.bytes = std::move(bytes);
    result.image.size_of_headers = size_headers;
    result.image.size_of_image = static_cast<std::uint32_t>(size_image64);
    result.image.has_nx_compat = true;
    return result;
}

} // namespace xfrontend
