// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/pe64_dll.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace xfrontend {
namespace {

constexpr std::uint32_t kDosHeaderSize = 0x80U;
constexpr std::uint32_t kPeOffsetField = 0x3cU;
constexpr std::uint32_t kCoffHeaderSize = 20U;
constexpr std::uint32_t kOptionalHeaderSize = 240U;
constexpr std::uint32_t kSectionHeaderSize = 40U;
constexpr std::uint16_t kMachineAmd64 = 0x8664U;
constexpr std::uint16_t kPe32PlusMagic = 0x020bU;
constexpr std::uint16_t kExecutableImage = 0x0002U;
constexpr std::uint16_t kLargeAddressAware = 0x0020U;
constexpr std::uint16_t kRelocationsStripped = 0x0001U;
constexpr std::uint16_t kDll = 0x2000U;
constexpr std::uint16_t kDynamicBase = 0x0040U;
constexpr std::uint16_t kNxCompat = 0x0100U;
constexpr std::uint32_t kTextCharacteristics = 0x60000020U;
constexpr std::uint32_t kReadOnlyCharacteristics = 0x40000040U;
constexpr std::uint32_t kReadWriteCharacteristics = 0xc0000040U;
constexpr std::uint32_t kRelocReadCharacteristics = 0x42000040U;
constexpr std::uint16_t kRelocAbsolute = 0U;
constexpr std::uint16_t kRelocDir64 = 10U;
constexpr std::size_t kMaximumExports = 4096U;
constexpr std::size_t kMaximumRelocations = 65536U;
constexpr std::size_t kMaximumImports = 4096U;
constexpr std::size_t kMaximumTlsTemplateBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumTlsZeroFillBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumTlsCallbacks = 1024U;
constexpr std::size_t kMaximumRuntimeFunctions = 65536U;
constexpr std::size_t kMaximumUnwindInfoBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t kOrdinalFlag64 = 0x8000000000000000ULL;

struct SectionBuild {
    Pe64DllSectionInfo info;
    std::vector<std::uint8_t> raw;
};

struct OrdinalBinding {
    Pe64SectionKind section = Pe64SectionKind::Text;
    std::size_t offset = 0U;
    bool forwarded = false;
    std::string forwarder;
};

struct ImportLibrary {
    std::string canonical_name;
    std::string original_name;
    std::vector<std::size_t> import_indices;
};

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

void append_string(std::vector<std::uint8_t>& bytes, const std::string& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0U);
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

bool add_signed_address(std::uint64_t base, std::int64_t addend, std::uint64_t& value) {
    if (addend >= 0) {
        const std::uint64_t amount = static_cast<std::uint64_t>(addend);
        if (base > std::numeric_limits<std::uint64_t>::max() - amount) return false;
        value = base + amount;
        return true;
    }
    const std::uint64_t amount = static_cast<std::uint64_t>(-(addend + 1)) + 1U;
    if (base < amount) return false;
    value = base - amount;
    return true;
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

bool fits_u32(std::uint64_t value) {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

bool fits_i32(std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

std::string ascii_lower(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

bool valid_ascii_name(const std::string& value, bool allow_empty) {
    if (value.empty()) return allow_empty;
    if (value.size() > 4096U) return false;
    for (const unsigned char byte : value) {
        if (byte < 0x21U || byte > 0x7eU || byte == 0x7fU) return false;
    }
    return value.find('\0') == std::string::npos;
}


bool is_nonvolatile_gpr(std::uint8_t reg) {
    return reg == 3U || reg == 5U || reg == 6U || reg == 7U || (reg >= 12U && reg <= 15U);
}

bool is_nonvolatile_xmm(std::uint8_t reg) {
    return reg >= 6U && reg <= 15U;
}

bool append_unwind_record(const Pe64DllRuntimeFunction& function,
                          std::vector<std::uint8_t>& xdata, std::string& error) {
    std::vector<std::uint8_t> code_slots;
    std::size_t slot_count = 0U;
    std::uint8_t previous_code_offset = std::numeric_limits<std::uint8_t>::max();
    bool has_set_frame = false;
    const auto append_slot = [&code_slots](std::uint8_t first, std::uint8_t second) {
        code_slots.push_back(first);
        code_slots.push_back(second);
    };
    const auto append_word = [&append_slot](std::uint16_t word) {
        append_slot(static_cast<std::uint8_t>(word & 0xffU), static_cast<std::uint8_t>((word >> 8U) & 0xffU));
    };

    for (const auto& code : function.unwind_codes) {
        if (code.code_offset == 0U || code.code_offset > function.prolog_size ||
            code.code_offset >= previous_code_offset) {
            error = "UNWIND_CODE CodeOffset values must be unique, descending and inside the prologue.";
            return false;
        }
        previous_code_offset = code.code_offset;
        const std::uint8_t operation = static_cast<std::uint8_t>(code.operation);
        const std::uint8_t operation_info = code.operation_info;
        append_slot(code.code_offset, static_cast<std::uint8_t>((operation_info << 4U) | operation));
        ++slot_count;
        switch (code.operation) {
        case Pe64DllUnwindOperation::PushNonVol:
            if (!is_nonvolatile_gpr(operation_info) || code.operand != 0U) {
                error = "UWOP_PUSH_NONVOL requires a nonvolatile GPR in OpInfo and no operand.";
                return false;
            }
            break;
        case Pe64DllUnwindOperation::AllocLarge:
            if (operation_info == 0U) {
                if (code.operand < 136U || code.operand > 65535U * 8U || (code.operand & 7U) != 0U) {
                    error = "UWOP_ALLOC_LARGE OpInfo=0 requires an aligned allocation from 136 through 524280 bytes.";
                    return false;
                }
                append_word(static_cast<std::uint16_t>(code.operand / 8U));
                ++slot_count;
            } else if (operation_info == 1U) {
                if (code.operand < 136U || (code.operand & 7U) != 0U) {
                    error = "UWOP_ALLOC_LARGE OpInfo=1 requires an aligned allocation of at least 136 bytes.";
                    return false;
                }
                append_word(static_cast<std::uint16_t>(code.operand & 0xffffU));
                append_word(static_cast<std::uint16_t>((code.operand >> 16U) & 0xffffU));
                slot_count += 2U;
            } else {
                error = "UWOP_ALLOC_LARGE supports only OpInfo 0 or 1.";
                return false;
            }
            break;
        case Pe64DllUnwindOperation::AllocSmall:
            if (operation_info > 15U || code.operand != 0U) {
                error = "UWOP_ALLOC_SMALL requires OpInfo 0..15 and no extra operand.";
                return false;
            }
            break;
        case Pe64DllUnwindOperation::SetFramePointer:
            if (operation_info != 0U || code.operand != 0U || function.frame_register == 0U || has_set_frame) {
                error = "UWOP_SET_FPREG requires a configured frame register and may appear only once with zero OpInfo/operand.";
                return false;
            }
            has_set_frame = true;
            break;
        case Pe64DllUnwindOperation::SaveNonVol:
            if (!is_nonvolatile_gpr(operation_info) || code.operand > 65535U * 8U || (code.operand & 7U) != 0U) {
                error = "UWOP_SAVE_NONVOL requires a nonvolatile GPR and an 8-byte-scaled 16-bit offset.";
                return false;
            }
            append_word(static_cast<std::uint16_t>(code.operand / 8U));
            ++slot_count;
            break;
        case Pe64DllUnwindOperation::SaveNonVolFar:
            if (!is_nonvolatile_gpr(operation_info) || code.operand > std::numeric_limits<std::uint32_t>::max() ||
                (code.operand & 7U) != 0U) {
                error = "UWOP_SAVE_NONVOL_FAR requires a nonvolatile GPR and an aligned 32-bit offset.";
                return false;
            }
            append_word(static_cast<std::uint16_t>(code.operand & 0xffffU));
            append_word(static_cast<std::uint16_t>((code.operand >> 16U) & 0xffffU));
            slot_count += 2U;
            break;
        case Pe64DllUnwindOperation::SaveXmm128:
            if (!is_nonvolatile_xmm(operation_info) || code.operand > 65535U * 16U || (code.operand & 15U) != 0U) {
                error = "UWOP_SAVE_XMM128 requires XMM6..XMM15 and a 16-byte-scaled 16-bit offset.";
                return false;
            }
            append_word(static_cast<std::uint16_t>(code.operand / 16U));
            ++slot_count;
            break;
        case Pe64DllUnwindOperation::SaveXmm128Far:
            if (!is_nonvolatile_xmm(operation_info) || code.operand > std::numeric_limits<std::uint32_t>::max() ||
                (code.operand & 15U) != 0U) {
                error = "UWOP_SAVE_XMM128_FAR requires XMM6..XMM15 and an aligned 32-bit offset.";
                return false;
            }
            append_word(static_cast<std::uint16_t>(code.operand & 0xffffU));
            append_word(static_cast<std::uint16_t>((code.operand >> 16U) & 0xffffU));
            slot_count += 2U;
            break;
        case Pe64DllUnwindOperation::PushMachineFrame:
            if (operation_info > 1U || code.operand != 0U) {
                error = "UWOP_PUSH_MACHFRAME requires OpInfo 0 or 1 and no extra operand.";
                return false;
            }
            break;
        default:
            error = "Unsupported or reserved AMD64 UNWIND_CODE operation.";
            return false;
        }
        if (slot_count > 255U) {
            error = "UNWIND_INFO CountOfCodes exceeds the one-byte 255-slot limit.";
            return false;
        }
    }

    if (function.frame_register == 0U) {
        if (function.frame_offset != 0U || has_set_frame) {
            error = "FrameOffset must be zero when no frame register is configured.";
            return false;
        }
    } else if (!is_nonvolatile_gpr(function.frame_register) || function.frame_offset > 15U || !has_set_frame) {
        error = "Frame register must be a nonvolatile GPR, FrameOffset must be 0..15, and UWOP_SET_FPREG is required.";
        return false;
    }

    if (function.flags > 4U || (function.flags == 4U && function.handler_offset != 0U) ||
        (function.flags == 4U && !function.handler_data.empty()) ||
        (function.flags != 4U && function.chained_function_index != static_cast<std::size_t>(-1)) ||
        (function.flags != 4U && function.flags != 0U && function.handler_offset == static_cast<std::size_t>(-1))) {
        error = "UNWIND_INFO handler/chained flags or associated metadata are inconsistent.";
        return false;
    }
    while ((xdata.size() & 3U) != 0U) xdata.push_back(0U);
    xdata.push_back(static_cast<std::uint8_t>(1U | (function.flags << 3U))); // Version 1 + UNW_FLAG_*.
    xdata.push_back(function.prolog_size);
    xdata.push_back(static_cast<std::uint8_t>(slot_count));
    xdata.push_back(static_cast<std::uint8_t>(function.frame_register | (function.frame_offset << 4U)));
    xdata.insert(xdata.end(), code_slots.begin(), code_slots.end());
    if ((slot_count & 1U) != 0U) { xdata.push_back(0U); xdata.push_back(0U); }
    while ((xdata.size() & 3U) != 0U) xdata.push_back(0U);
    if ((function.flags & 4U) != 0U) {
        xdata.resize(xdata.size() + 12U, 0U); // CHAININFO RUNTIME_FUNCTION triple, patched after RVAs are known.
    } else if ((function.flags & 3U) != 0U) {
        xdata.resize(xdata.size() + 4U, 0U); // Exception/termination handler RVA, patched after RVAs are known.
        xdata.insert(xdata.end(), function.handler_data.begin(), function.handler_data.end());
        while ((xdata.size() & 3U) != 0U) xdata.push_back(0U);
    }
    if (xdata.size() > kMaximumUnwindInfoBytes) {
        error = "Combined .xdata UNWIND_INFO exceeds the 16-MiB safety limit.";
        return false;
    }
    return true;
}

void add_diagnostic(Pe64DllResult& result, const char* code, const std::string& message) {
    result.diagnostics.push_back(Diagnostic{code, message, SourceSpan{}});
}

bool section_rva(const Pe64DllSpec& spec,
                 const std::vector<Pe64DllSectionInfo>& sections,
                 Pe64SectionKind kind, std::size_t offset, std::uint32_t& rva) {
    const std::vector<std::uint8_t>* bytes = nullptr;
    const char* name = nullptr;
    switch (kind) {
    case Pe64SectionKind::Text: bytes = &spec.text; name = ".text"; break;
    case Pe64SectionKind::ReadOnlyData: bytes = &spec.read_only_data; name = ".rdata"; break;
    case Pe64SectionKind::Data: bytes = &spec.data; name = ".data"; break;
    default: return false;
    }
    if (offset >= bytes->size()) return false;
    const auto found = std::find_if(sections.begin(), sections.end(), [name](const Pe64DllSectionInfo& item) {
        return item.name == name;
    });
    if (found == sections.end()) return false;
    const std::uint64_t value = static_cast<std::uint64_t>(found->virtual_address) + offset;
    if (!fits_u32(value)) return false;
    rva = static_cast<std::uint32_t>(value);
    return true;
}

std::vector<std::uint8_t> build_import_section(const Pe64DllSpec& spec,
                                               std::uint32_t idata_rva,
                                               Pe64DllImage& image,
                                               std::vector<std::uint32_t>& iat_rvas) {
    iat_rvas.assign(spec.imports.size(), 0U);
    image.import_directory_rva = 0U;
    image.import_directory_size = 0U;
    image.iat_rva = 0U;
    image.iat_size = 0U;
    image.imported_symbol_count = 0U;
    image.imported_name_count = 0U;
    image.imported_ordinal_count = 0U;
    image.imported_library_count = 0U;
    if (spec.imports.empty()) return {};

    std::vector<ImportLibrary> libraries;
    std::map<std::string, std::size_t> library_map;
    for (std::size_t i = 0U; i < spec.imports.size(); ++i) {
        const auto& item = spec.imports[i];
        const std::string key = ascii_lower(item.dll_name);
        const auto found = library_map.find(key);
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
    const std::size_t ilt_base_size = (descriptor_size + 7U) & ~std::size_t{7U};
    std::size_t ilt_total = 0U;
    for (const auto& library : libraries) {
        const std::size_t thunk_count = library.import_indices.size() + 1U;
        if (thunk_count > (std::numeric_limits<std::size_t>::max() - ilt_total) / 8U)
            return {};
        ilt_total += thunk_count * 8U;
    }
    if (ilt_base_size > std::numeric_limits<std::size_t>::max() - ilt_total)
        return {};
    const std::size_t iat_base_size = ilt_base_size + ilt_total;
    if (iat_base_size > std::numeric_limits<std::size_t>::max() - ilt_total)
        return {};
    bytes.resize(iat_base_size + ilt_total, 0U);

    std::size_t ilt_cursor = ilt_base_size;
    std::size_t iat_cursor = iat_base_size;
    std::vector<std::size_t> descriptor_offsets;
    descriptor_offsets.reserve(libraries.size());
    for (std::size_t library_index = 0U; library_index < libraries.size(); ++library_index) {
        const auto& library = libraries[library_index];
        const std::size_t descriptor = library_index * 20U;
        descriptor_offsets.push_back(descriptor);
        const std::uint64_t ilt_rva = static_cast<std::uint64_t>(idata_rva) + ilt_cursor;
        const std::uint64_t iat_rva = static_cast<std::uint64_t>(idata_rva) + iat_cursor;
        if (!fits_u32(ilt_rva) || !fits_u32(iat_rva)) return {};
        put32(bytes, descriptor, static_cast<std::uint32_t>(ilt_rva));
        put32(bytes, descriptor + 16U, static_cast<std::uint32_t>(iat_rva));

        for (const std::size_t import_index : library.import_indices) {
            const auto& imported = spec.imports[import_index];
            std::uint64_t thunk_value = 0U;
            if (imported.kind == Pe64ImportKind::ByOrdinal) {
                thunk_value = kOrdinalFlag64 | static_cast<std::uint64_t>(imported.ordinal);
                ++image.imported_ordinal_count;
            } else {
                ++image.imported_name_count;
                const std::size_t name_pos = (bytes.size() + 1U) & ~std::size_t{1U};
                if (bytes.size() < name_pos) bytes.resize(name_pos, 0U);
                const std::uint64_t ibn_rva = static_cast<std::uint64_t>(idata_rva) + name_pos;
                if (!fits_u32(ibn_rva)) return {};
                append16(bytes, imported.hint);
                append_string(bytes, imported.symbol_name);
                thunk_value = ibn_rva;
            }
            put64(bytes, ilt_cursor, thunk_value);
            put64(bytes, iat_cursor, thunk_value);
            const std::uint64_t slot_rva = static_cast<std::uint64_t>(idata_rva) + iat_cursor;
            if (!fits_u32(slot_rva)) return {};
            iat_rvas[import_index] = static_cast<std::uint32_t>(slot_rva);
            ilt_cursor += 8U;
            iat_cursor += 8U;
        }
        // Zero terminators for each ILT and IAT are reserved in the arrays.
        ilt_cursor += 8U;
        iat_cursor += 8U;
    }

    for (std::size_t library_index = 0U; library_index < libraries.size(); ++library_index) {
        const std::size_t name_pos = bytes.size();
        const std::uint64_t name_rva = static_cast<std::uint64_t>(idata_rva) + name_pos;
        if (!fits_u32(name_rva)) return {};
        append_string(bytes, libraries[library_index].original_name);
        put32(bytes, descriptor_offsets[library_index] + 12U, static_cast<std::uint32_t>(name_rva));
    }

    const std::uint64_t directory_size = descriptor_size;
    const std::uint64_t iat_base_rva = static_cast<std::uint64_t>(idata_rva) + iat_base_size;
    if (!fits_u32(directory_size) || !fits_u32(iat_base_rva) || !fits_u32(ilt_total) ||
        static_cast<std::uint64_t>(idata_rva) + bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return {};
    image.import_directory_rva = idata_rva;
    image.import_directory_size = static_cast<std::uint32_t>(directory_size);
    image.iat_rva = static_cast<std::uint32_t>(iat_base_rva);
    image.iat_size = static_cast<std::uint32_t>(ilt_total);
    image.imported_symbol_count = spec.imports.size();
    image.imported_library_count = libraries.size();
    return bytes;
}

std::uint32_t section_characteristics(const std::string& name) {
    if (name == ".text") return kTextCharacteristics;
    if (name == ".data" || name == ".tls") return kReadWriteCharacteristics;
    return kReadOnlyCharacteristics;
}

} // namespace

Pe64DllResult write_pe64_dll_image(const Pe64DllSpec& spec) {
    Pe64DllResult result;
    if (!valid_ascii_name(spec.dll_name, false) || spec.dll_name.size() > 260U) {
        add_diagnostic(result, "XPE64DLL0001", "DLL name must be a non-empty printable ASCII string no longer than 260 bytes.");
        return result;
    }
    if (spec.text.empty()) {
        add_diagnostic(result, "XPE64DLL0002", "DLL .text section must not be empty.");
        return result;
    }
    if (spec.exports.empty() || spec.exports.size() > kMaximumExports) {
        add_diagnostic(result, "XPE64DLL0003", "Export count must be between 1 and 4096.");
        return result;
    }
    if (!is_power_of_two(spec.section_alignment) || !is_power_of_two(spec.file_alignment) ||
        spec.file_alignment < 0x200U || spec.file_alignment > 0x10000U ||
        spec.section_alignment < spec.file_alignment) {
        add_diagnostic(result, "XPE64DLL0004", "PE section/file alignments are invalid.");
        return result;
    }
    if (spec.image_base == 0U || (spec.image_base & 0xffffU) != 0U) {
        add_diagnostic(result, "XPE64DLL0005", "PE32+ image base must be non-zero and 64-KiB aligned.");
        return result;
    }
    if (spec.maximum_file_size < 0x400U || spec.maximum_file_size > 1024ULL * 1024ULL * 1024ULL) {
        add_diagnostic(result, "XPE64DLL0006", "Maximum file size is outside the supported 1 KiB..1 GiB range.");
        return result;
    }

    if (spec.absolute_relocations.size() > kMaximumRelocations) {
        add_diagnostic(result, "XPE64DLL0028", "Base-relocation count exceeds the 65536-entry safety limit.");
        return result;
    }
    std::set<std::size_t> relocation_destinations;
    for (const auto& relocation : spec.absolute_relocations) {
        if ((relocation.destination_offset & 7U) != 0U ||
            relocation.destination_offset > spec.data.size() ||
            spec.data.size() - relocation.destination_offset < 8U) {
            add_diagnostic(result, "XPE64DLL0029", "DIR64 destination must be an aligned initialized .data qword.");
            return result;
        }
        if (!relocation_destinations.insert(relocation.destination_offset).second) {
            add_diagnostic(result, "XPE64DLL0030", "Duplicate or overlapping DIR64 destinations are not permitted.");
            return result;
        }
        const std::vector<std::uint8_t>* target = nullptr;
        switch (relocation.target_section) {
        case Pe64SectionKind::Text: target = &spec.text; break;
        case Pe64SectionKind::ReadOnlyData: target = &spec.read_only_data; break;
        case Pe64SectionKind::Data: target = &spec.data; break;
        default: target = nullptr; break;
        }
        if (target == nullptr || relocation.target_offset >= target->size()) {
            add_diagnostic(result, "XPE64DLL0031", "DIR64 target must point inside .text, .rdata or initialized .data.");
            return result;
        }
    }

    if (spec.has_entry_point && spec.entry_point_offset >= spec.text.size()) {
        add_diagnostic(result, "XPE64DLL0060", "DLL entry point must target a valid byte inside executable .text.");
        return result;
    }

    // Validate and pre-encode optional AMD64 runtime-function records.
    // Check the function count before allocating/sorting pointers.
    std::vector<const Pe64DllRuntimeFunction*> sorted_runtime_functions;
    std::vector<std::uint32_t> unwind_record_offsets;
    std::vector<std::uint8_t> xdata_bytes;
    if (spec.runtime_functions.size() > kMaximumRuntimeFunctions) {
        add_diagnostic(result, "XPE64DLL0061", "Runtime function count exceeds the 65536-entry safety limit.");
        return result;
    }
    if (!spec.runtime_functions.empty()) {
        sorted_runtime_functions.reserve(spec.runtime_functions.size());
        for (const auto& function : spec.runtime_functions) sorted_runtime_functions.push_back(&function);
        std::sort(sorted_runtime_functions.begin(), sorted_runtime_functions.end(),
            [](const Pe64DllRuntimeFunction* left, const Pe64DllRuntimeFunction* right) {
                if (left->begin_offset != right->begin_offset) return left->begin_offset < right->begin_offset;
                return left->end_offset < right->end_offset;
            });
        for (std::size_t index = 0U; index < sorted_runtime_functions.size(); ++index) {
            const auto& function = *sorted_runtime_functions[index];
            if (function.begin_offset >= function.end_offset || function.end_offset > spec.text.size() ||
                function.prolog_size > function.end_offset - function.begin_offset) {
                add_diagnostic(result, "XPE64DLL0062", "Runtime function range or prologue size is invalid or outside .text.");
                return result;
            }
            if (index > 0U && function.begin_offset < sorted_runtime_functions[index - 1U]->end_offset) {
                add_diagnostic(result, "XPE64DLL0062", "Runtime function ranges must not overlap.");
                return result;
            }
            if (function.flags > 4U || ((function.flags & 4U) != 0U && (function.flags & 3U) != 0U)) {
                add_diagnostic(result, "XPE64DLL0063", "UNWIND_INFO flags contain reserved bits or combine CHAININFO with a handler.");
                return result;
            }
            if ((function.flags & 3U) != 0U && function.handler_offset >= spec.text.size()) {
                add_diagnostic(result, "XPE64DLL0063", "Exception/termination handler must target a valid .text byte.");
                return result;
            }
            if ((function.flags & 4U) != 0U && (function.chained_function_index >= spec.runtime_functions.size() ||
                &spec.runtime_functions[function.chained_function_index] == &function)) {
                add_diagnostic(result, "XPE64DLL0063", "CHAININFO must reference a different runtime-function record by valid input index.");
                return result;
            }
            if ((function.flags & 4U) == 0U && function.chained_function_index != static_cast<std::size_t>(-1)) {
                add_diagnostic(result, "XPE64DLL0063", "A chained function index is valid only with UNW_FLAG_CHAININFO.");
                return result;
            }
            if ((function.flags & 3U) == 0U && (!function.handler_data.empty() || function.handler_offset != 0U)) {
                add_diagnostic(result, "XPE64DLL0063", "Handler target/data requires EHANDLER or UHANDLER flags.");
                return result;
            }
            unwind_record_offsets.push_back(static_cast<std::uint32_t>(xdata_bytes.size()));
            std::string unwind_error;
            if (!append_unwind_record(function, xdata_bytes, unwind_error)) {
                add_diagnostic(result, "XPE64DLL0063", unwind_error);
                return result;
            }
        }
    }

    if (!sorted_runtime_functions.empty()) {
        // CHAININFO forms a directed graph with at most one outgoing edge per
        // runtime function. Reject cycles before producing loader metadata.
        std::vector<std::uint8_t> chain_state(spec.runtime_functions.size(), 0U);
        for (std::size_t start = 0U; start < spec.runtime_functions.size(); ++start) {
            if (chain_state[start] == 2U) continue;
            std::vector<std::size_t> path;
            std::size_t current = start;
            while (current < spec.runtime_functions.size() && chain_state[current] == 0U) {
                chain_state[current] = 1U;
                path.push_back(current);
                const auto& function = spec.runtime_functions[current];
                current = (function.flags & 4U) != 0U ? function.chained_function_index : spec.runtime_functions.size();
            }
            if (current < spec.runtime_functions.size() && chain_state[current] == 1U) {
                add_diagnostic(result, "XPE64DLL0069", "UNW_FLAG_CHAININFO references contain a cycle.");
                return result;
            }
            for (const auto item : path) chain_state[item] = 2U;
        }
    }

    if (spec.tls.enabled) {
        if (spec.tls.template_data.empty() && spec.tls.zero_fill_size == 0U) {
            add_diagnostic(result, "XPE64DLL0050", "Enabled TLS requires initialized template bytes or a non-zero zero-fill size.");
            return result;
        }
        if (spec.tls.template_data.size() > kMaximumTlsTemplateBytes ||
            spec.tls.zero_fill_size > kMaximumTlsZeroFillBytes) {
            add_diagnostic(result, "XPE64DLL0051", "TLS initialized or zero-fill template exceeds the 16-MiB safety limit.");
            return result;
        }
        if (spec.tls.index_data_offset > spec.data.size() ||
            spec.data.size() - spec.tls.index_data_offset < 4U ||
            (spec.tls.index_data_offset & 3U) != 0U) {
            add_diagnostic(result, "XPE64DLL0052", "TLS index must reference an aligned four-byte slot inside initialized .data.");
            return result;
        }
        if (std::any_of(spec.data.begin() + static_cast<std::ptrdiff_t>(spec.tls.index_data_offset),
                        spec.data.begin() + static_cast<std::ptrdiff_t>(spec.tls.index_data_offset + 4U),
                        [](std::uint8_t value) { return value != 0U; })) {
            add_diagnostic(result, "XPE64DLL0053", "TLS index storage must be zero-initialized for the Windows loader.");
            return result;
        }
        if (spec.tls.callbacks.size() > kMaximumTlsCallbacks) {
            add_diagnostic(result, "XPE64DLL0054", "TLS callback count exceeds the 1024-entry safety limit.");
            return result;
        }
        std::set<std::size_t> callback_offsets;
        for (const auto& callback : spec.tls.callbacks) {
            if (callback.target_section != Pe64SectionKind::Text || callback.target_offset >= spec.text.size()) {
                add_diagnostic(result, "XPE64DLL0055", "TLS callbacks must target a valid offset inside executable .text.");
                return result;
            }
            if (!callback_offsets.insert(callback.target_offset).second) {
                add_diagnostic(result, "XPE64DLL0056", "Duplicate TLS callback targets are not permitted.");
                return result;
            }
        }
        for (const auto& relocation : spec.absolute_relocations) {
            const std::size_t relocation_end = relocation.destination_offset + 8U;
            const std::size_t index_end = spec.tls.index_data_offset + 4U;
            if (relocation.destination_offset < index_end && spec.tls.index_data_offset < relocation_end) {
                add_diagnostic(result, "XPE64DLL0057", "TLS index storage must not overlap a user DIR64 relocation destination.");
                return result;
            }
        }
    }

    if (spec.imports.size() > kMaximumImports) {
        add_diagnostic(result, "XPE64DLL0034", "Import count exceeds the 4096-entry safety limit.");
        return result;
    }
    using ImportKey = std::tuple<std::string, std::uint8_t, std::string, std::uint16_t>;
    std::set<ImportKey> import_keys;
    for (const auto& item : spec.imports) {
        if (!valid_ascii_name(item.dll_name, false) || item.dll_name.size() > 260U) {
            add_diagnostic(result, "XPE64DLL0035", "Import DLL name must be printable ASCII, non-empty and no longer than 260 bytes.");
            return result;
        }
        if (item.kind == Pe64ImportKind::ByName) {
            if (!valid_ascii_name(item.symbol_name, false) || item.symbol_name.size() > 4096U || item.ordinal != 0U) {
                add_diagnostic(result, "XPE64DLL0035", "Name import requires a printable ASCII symbol, a maximum of 4096 bytes and an unused zero ordinal field.");
                return result;
            }
        } else if (item.kind == Pe64ImportKind::ByOrdinal) {
            if (!item.symbol_name.empty() || item.hint != 0U || item.ordinal == 0U) {
                add_diagnostic(result, "XPE64DLL0036", "Ordinal import requires ordinal 1..65535 and no symbol name or hint.");
                return result;
            }
        } else {
            add_diagnostic(result, "XPE64DLL0036", "Unknown PE import kind.");
            return result;
        }
        const std::uint16_t key_ordinal = item.kind == Pe64ImportKind::ByOrdinal ? item.ordinal : 0U;
        const ImportKey key{ascii_lower(item.dll_name), static_cast<std::uint8_t>(item.kind),
                            item.symbol_name, key_ordinal};
        if (!import_keys.insert(key).second) {
            add_diagnostic(result, "XPE64DLL0037", "Duplicate DLL/symbol-or-ordinal import is not permitted.");
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
        if ((call.instruction_kind != Pe64IatInstructionKind::CallIndirect &&
             call.instruction_kind != Pe64IatInstructionKind::JumpIndirect) ||
            (call.import_kind != Pe64ImportKind::ByName && call.import_kind != Pe64ImportKind::ByOrdinal)) {
            add_diagnostic(result, "XPE64DLL0038", "Unknown IAT instruction or import reference kind.");
            return result;
        }
        const std::uint8_t expected_modrm = call.instruction_kind == Pe64IatInstructionKind::CallIndirect ? 0x15U : 0x25U;
        if (call.displacement_offset < 2U || call.displacement_offset > spec.text.size() ||
            spec.text.size() - call.displacement_offset < 4U ||
            spec.text[call.displacement_offset - 2U] != 0xffU ||
            spec.text[call.displacement_offset - 1U] != expected_modrm) {
            add_diagnostic(result, "XPE64DLL0038", "IAT site must identify the disp32 field of the selected FF 15/FF 25 instruction.");
            return result;
        }
        if (call.import_kind == Pe64ImportKind::ByOrdinal && !call.symbol_name.empty()) {
            add_diagnostic(result, "XPE64DLL0038", "Ordinal IAT call site must not carry a symbol name.");
            return result;
        }
        if (call.import_kind == Pe64ImportKind::ByName && (call.symbol_name.empty() || call.ordinal != 0U)) {
            add_diagnostic(result, "XPE64DLL0038", "Name IAT call site requires a symbol name and an unused zero ordinal field.");
            return result;
        }
        const auto found = find_import(call);
        if (found == spec.imports.end()) {
            add_diagnostic(result, "XPE64DLL0039", "IAT call/jump site references an import that is not declared.");
            return result;
        }
        call_site_ranges.emplace_back(call.displacement_offset - 2U, call.displacement_offset + 4U);
    }
    std::sort(call_site_ranges.begin(), call_site_ranges.end());
    for (std::size_t i = 1U; i < call_site_ranges.size(); ++i) {
        if (call_site_ranges[i].first < call_site_ranges[i - 1U].second) {
            add_diagnostic(result, "XPE64DLL0040", "Overlapping IAT call-site instructions are not permitted.");
            return result;
        }
    }
    if (spec.imports.empty() && !spec.iat_call_sites.empty()) {
        add_diagnostic(result, "XPE64DLL0039", "IAT call sites require at least one declared import.");
        return result;
    }

    std::map<std::uint32_t, OrdinalBinding> bindings;
    std::map<std::string, std::uint32_t> names;
    std::uint32_t ordinal_base = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_ordinal = 0U;
    std::size_t named_count = 0U;
    for (const auto& item : spec.exports) {
        if (item.ordinal == 0U || item.ordinal > 65535U) {
            add_diagnostic(result, "XPE64DLL0007", "Export ordinal must be in the range 1..65535.");
            return result;
        }
        if (!valid_ascii_name(item.name, true) || (!item.name.empty() && item.name.find('=') != std::string::npos)) {
            add_diagnostic(result, "XPE64DLL0008", "Export names must be printable ASCII without '='.");
            return result;
        }
        if (item.forwarded) {
            const std::size_t separator = item.forwarder.find('.');
            if (!valid_ascii_name(item.forwarder, false) || separator == std::string::npos ||
                separator == 0U || separator + 1U >= item.forwarder.size()) {
                add_diagnostic(result, "XPE64DLL0009", "Forwarder must be a non-empty ASCII DLL.symbol or DLL.#ordinal string.");
                return result;
            }
        } else {
            const std::vector<std::uint8_t>* target = nullptr;
            switch (item.target_section) {
            case Pe64SectionKind::Text: target = &spec.text; break;
            case Pe64SectionKind::ReadOnlyData: target = &spec.read_only_data; break;
            case Pe64SectionKind::Data: target = &spec.data; break;
            default: target = nullptr; break;
            }
            if (target == nullptr || item.target_offset >= target->size()) {
                add_diagnostic(result, "XPE64DLL0010", "Export target section/offset is invalid.");
                return result;
            }
        }
        if (!item.name.empty()) {
            ++named_count;
            if (!names.emplace(item.name, item.ordinal).second) {
                add_diagnostic(result, "XPE64DLL0011", "Duplicate export name is not permitted.");
                return result;
            }
        }
        const OrdinalBinding binding{item.target_section, item.target_offset, item.forwarded, item.forwarder};
        const auto inserted = bindings.emplace(item.ordinal, binding);
        if (!inserted.second) {
            const auto& existing = inserted.first->second;
            if (existing.section != binding.section || existing.offset != binding.offset ||
                existing.forwarded != binding.forwarded || existing.forwarder != binding.forwarder) {
                add_diagnostic(result, "XPE64DLL0012", "Aliases sharing an ordinal must resolve to the same target.");
                return result;
            }
        }
        ordinal_base = std::min(ordinal_base, item.ordinal);
        maximum_ordinal = std::max(maximum_ordinal, item.ordinal);
    }
    const std::uint64_t function_count64 = static_cast<std::uint64_t>(maximum_ordinal) - ordinal_base + 1U;
    if (function_count64 == 0U || function_count64 > kMaximumExports) {
        add_diagnostic(result, "XPE64DLL0013", "Export ordinal span exceeds 4096 slots.");
        return result;
    }
    const std::uint32_t function_count = static_cast<std::uint32_t>(function_count64);

    // Reserve TLS metadata in the writable .data section before assigning RVAs.
    // The TLS index itself remains caller-owned at tls.index_data_offset.
    std::vector<std::uint8_t> effective_data = spec.data;
    std::vector<std::uint8_t> tls_section_data;
    std::size_t tls_directory_offset = 0U;
    std::size_t tls_callbacks_offset = 0U;
    std::size_t tls_relocation_count = 0U;
    if (spec.tls.enabled) {
        tls_section_data = spec.tls.template_data;
        if (tls_section_data.empty()) tls_section_data.push_back(0U); // zero-length template anchor
        while ((effective_data.size() & 7U) != 0U) effective_data.push_back(0U);
        tls_directory_offset = effective_data.size();
        effective_data.resize(effective_data.size() + 40U, 0U); // IMAGE_TLS_DIRECTORY64
        if (!spec.tls.callbacks.empty()) {
            while ((effective_data.size() & 7U) != 0U) effective_data.push_back(0U);
            tls_callbacks_offset = effective_data.size();
            effective_data.resize(effective_data.size() + (spec.tls.callbacks.size() + 1U) * 8U, 0U);
        }
        // Three mandatory VAs (template start/end and index), plus callback-array
        // VA and one VA relocation for every non-null callback entry.
        tls_relocation_count = 3U + (spec.tls.callbacks.empty() ? 0U : 1U + spec.tls.callbacks.size());
    }

    // .edata content is built after RVAs for the non-export sections are known.
    std::vector<SectionBuild> sections;
    const auto add_section = [&sections](const char* name, const std::vector<std::uint8_t>& data) {
        if (data.empty()) return;
        SectionBuild section;
        section.info.name = name;
        section.info.virtual_size = static_cast<std::uint32_t>(data.size());
        section.info.characteristics = section_characteristics(name);
        section.raw = data;
        sections.push_back(std::move(section));
    };
    add_section(".text", spec.text);
    add_section(".rdata", spec.read_only_data);
    add_section(".data", effective_data);
    if (spec.tls.enabled) add_section(".tls", tls_section_data);

    // IMAGE_FILE_HEADER.NumberOfSections is a 16-bit WORD. Keep the range
    // check next to the narrowing conversion so strict MSVC /W4 /WX builds
    // cannot silently truncate a future expanded section list.
    const bool has_any_relocations = !spec.absolute_relocations.empty() || spec.tls.enabled;
    const std::size_t section_count_size = sections.size() + 1U +
        (spec.imports.empty() ? 0U : 1U) +
        (has_any_relocations ? 1U : 0U) +
        (spec.runtime_functions.empty() ? 0U : 2U); // .edata plus optional .idata/.reloc/.pdata/.xdata
    if (section_count_size > std::numeric_limits<std::uint16_t>::max()) {
        add_diagnostic(result, "XPE64DLL0027", "Section count exceeds the PE COFF header limit.");
        return result;
    }
    const std::uint16_t section_count = static_cast<std::uint16_t>(section_count_size);
    // Reserve header-table capacity for the five N39-N42 advanced PE sections
    // (.rsrc, .bnd, .didat, .lcfg and merged .relx). NumberOfSections still
    // records only sections that are physically present in this base image.
    constexpr std::size_t kAdvancedSectionHeaderReserve = 5U;
    if (section_count_size > std::numeric_limits<std::uint16_t>::max() - kAdvancedSectionHeaderReserve) {
        add_diagnostic(result, "XPE64DLL0027", "PE section count plus advanced-header reserve exceeds the COFF header limit.");
        return result;
    }
    const std::size_t header_section_capacity = section_count_size + kAdvancedSectionHeaderReserve;
    const std::uint64_t header_end = static_cast<std::uint64_t>(kDosHeaderSize) + 4U +
        kCoffHeaderSize + kOptionalHeaderSize + static_cast<std::uint64_t>(header_section_capacity) * kSectionHeaderSize;
    std::uint64_t size_of_headers64 = 0U;
    if (!checked_align(header_end, spec.file_alignment, size_of_headers64) || !fits_u32(size_of_headers64)) {
        add_diagnostic(result, "XPE64DLL0014", "PE headers exceed the supported address range.");
        return result;
    }

    std::uint64_t next_rva = 0U;
    if (!checked_align(size_of_headers64, spec.section_alignment, next_rva)) {
        add_diagnostic(result, "XPE64DLL0015", "Unable to align first section RVA.");
        return result;
    }
    for (auto& section : sections) {
        std::uint64_t aligned = 0U;
        if (!checked_align(next_rva, spec.section_alignment, aligned) || !fits_u32(aligned)) {
            add_diagnostic(result, "XPE64DLL0016", "Section RVA exceeds PE32+ RVA range.");
            return result;
        }
        section.info.virtual_address = static_cast<std::uint32_t>(aligned);
        if (section.raw.size() > std::numeric_limits<std::uint32_t>::max()) {
            add_diagnostic(result, "XPE64DLL0017", "Section is too large.");
            return result;
        }
        next_rva = aligned + section.info.virtual_size;
    }
    std::vector<Pe64DllSectionInfo> section_infos;
    for (const auto& section : sections) section_infos.push_back(section.info);

    std::uint32_t entry_point_rva = 0U;
    if (spec.has_entry_point &&
        !section_rva(spec, section_infos, Pe64SectionKind::Text, spec.entry_point_offset, entry_point_rva)) {
        add_diagnostic(result, "XPE64DLL0060", "DLL entry point could not be mapped into executable .text.");
        return result;
    }

    // Materialize preferred-base pointer values in initialized .data. The
    // Loader later applies the matching DIR64 entries if the DLL is rebased.
    if (!spec.absolute_relocations.empty()) {
        auto data_section = std::find_if(sections.begin(), sections.end(), [](const SectionBuild& item) {
            return item.info.name == ".data";
        });
        if (data_section == sections.end()) {
            add_diagnostic(result, "XPE64DLL0029", "DIR64 relocations require an initialized .data section.");
            return result;
        }
        for (const auto& relocation : spec.absolute_relocations) {
            std::uint32_t target_rva = 0U;
            if (!section_rva(spec, section_infos, relocation.target_section, relocation.target_offset, target_rva)) {
                add_diagnostic(result, "XPE64DLL0031", "DIR64 target could not be mapped to an image RVA.");
                return result;
            }
            if (spec.image_base > std::numeric_limits<std::uint64_t>::max() - target_rva) {
                add_diagnostic(result, "XPE64DLL0032", "DIR64 preferred-base address overflows the 64-bit address space.");
                return result;
            }
            std::uint64_t pointer_value = 0U;
            if (!add_signed_address(spec.image_base + static_cast<std::uint64_t>(target_rva),
                                   relocation.addend, pointer_value)) {
                add_diagnostic(result, "XPE64DLL0032", "DIR64 addend overflows or underflows the 64-bit address space.");
                return result;
            }
            put64(data_section->raw, relocation.destination_offset, pointer_value);
        }
    }

    std::uint32_t tls_directory_rva = 0U;
    std::uint32_t tls_index_rva = 0U;
    std::uint32_t tls_template_rva = 0U;
    if (spec.tls.enabled) {
        auto data_section = std::find_if(sections.begin(), sections.end(), [](const SectionBuild& item) {
            return item.info.name == ".data";
        });
        const auto tls_section = std::find_if(sections.begin(), sections.end(), [](const SectionBuild& item) {
            return item.info.name == ".tls";
        });
        if (data_section == sections.end() || tls_section == sections.end()) {
            add_diagnostic(result, "XPE64DLL0058", "TLS metadata requires writable .data and .tls sections.");
            return result;
        }
        const std::uint64_t directory_rva64 = static_cast<std::uint64_t>(data_section->info.virtual_address) + tls_directory_offset;
        const std::uint64_t index_rva64 = static_cast<std::uint64_t>(data_section->info.virtual_address) + spec.tls.index_data_offset;
        const std::uint64_t template_end_rva64 = static_cast<std::uint64_t>(tls_section->info.virtual_address) + spec.tls.template_data.size();
        const std::uint64_t callbacks_rva64 = spec.tls.callbacks.empty() ? 0U :
            static_cast<std::uint64_t>(data_section->info.virtual_address) + tls_callbacks_offset;
        if (!fits_u32(directory_rva64) || !fits_u32(index_rva64) ||
            !fits_u32(tls_section->info.virtual_address) || !fits_u32(template_end_rva64) ||
            (callbacks_rva64 != 0U && !fits_u32(callbacks_rva64))) {
            add_diagnostic(result, "XPE64DLL0058", "TLS directory, template, index or callback RVA exceeds the PE32+ range.");
            return result;
        }
        const auto va_from_rva = [&spec](std::uint32_t rva, std::uint64_t& va) {
            if (spec.image_base > std::numeric_limits<std::uint64_t>::max() - rva) return false;
            va = spec.image_base + rva;
            return true;
        };
        std::uint64_t template_start_va = 0U;
        std::uint64_t template_end_va = 0U;
        std::uint64_t index_va = 0U;
        std::uint64_t callbacks_va = 0U;
        if (!va_from_rva(tls_section->info.virtual_address, template_start_va) ||
            !va_from_rva(static_cast<std::uint32_t>(template_end_rva64), template_end_va) ||
            !va_from_rva(static_cast<std::uint32_t>(index_rva64), index_va) ||
            (!spec.tls.callbacks.empty() && !va_from_rva(static_cast<std::uint32_t>(callbacks_rva64), callbacks_va))) {
            add_diagnostic(result, "XPE64DLL0058", "TLS preferred-base VA calculation overflows the 64-bit address space.");
            return result;
        }
        put64(data_section->raw, tls_directory_offset, template_start_va);
        put64(data_section->raw, tls_directory_offset + 8U, template_end_va);
        put64(data_section->raw, tls_directory_offset + 16U, index_va);
        put64(data_section->raw, tls_directory_offset + 24U, callbacks_va);
        put32(data_section->raw, tls_directory_offset + 32U, spec.tls.zero_fill_size);
        put32(data_section->raw, tls_directory_offset + 36U, 0U); // default byte alignment
        for (std::size_t index = 0U; index < spec.tls.callbacks.size(); ++index) {
            std::uint32_t callback_rva = 0U;
            const auto& callback = spec.tls.callbacks[index];
            if (!section_rva(spec, section_infos, callback.target_section, callback.target_offset, callback_rva)) {
                add_diagnostic(result, "XPE64DLL0058", "TLS callback target could not be mapped to an image RVA.");
                return result;
            }
            std::uint64_t callback_va = 0U;
            if (!va_from_rva(callback_rva, callback_va)) {
                add_diagnostic(result, "XPE64DLL0058", "TLS callback preferred-base VA overflows the 64-bit address space.");
                return result;
            }
            put64(data_section->raw, tls_callbacks_offset + index * 8U, callback_va);
        }
        tls_directory_rva = static_cast<std::uint32_t>(directory_rva64);
        tls_index_rva = static_cast<std::uint32_t>(index_rva64);
        tls_template_rva = tls_section->info.virtual_address;
    }

    std::vector<std::uint8_t> edata(40U, 0U);
    const std::uint32_t dll_name_offset = static_cast<std::uint32_t>(edata.size());
    append_string(edata, spec.dll_name);
    while ((edata.size() & 3U) != 0U) edata.push_back(0U);
    const std::uint32_t eat_offset = static_cast<std::uint32_t>(edata.size());
    edata.resize(edata.size() + static_cast<std::size_t>(function_count) * 4U, 0U);
    const std::uint32_t name_ptr_offset = static_cast<std::uint32_t>(edata.size());
    edata.resize(edata.size() + named_count * 4U, 0U);
    const std::uint32_t ordinal_table_offset = static_cast<std::uint32_t>(edata.size());
    edata.resize(edata.size() + named_count * 2U, 0U);

    struct NamedExport { std::string name; std::uint32_t ordinal; };
    std::vector<NamedExport> named_exports;
    for (const auto& pair : names) named_exports.push_back(NamedExport{pair.first, pair.second});
    std::sort(named_exports.begin(), named_exports.end(), [](const NamedExport& left, const NamedExport& right) {
        return left.name < right.name;
    });
    std::vector<std::uint32_t> forwarder_offsets;
    for (const auto& pair : bindings) {
        if (pair.second.forwarded) {
            forwarder_offsets.push_back(pair.first);
        }
    }
    std::map<std::uint32_t, std::uint32_t> forwarder_string_offsets;
    for (const std::uint32_t ordinal : forwarder_offsets) {
        forwarder_string_offsets.emplace(ordinal, static_cast<std::uint32_t>(edata.size()));
        append_string(edata, bindings.at(ordinal).forwarder);
    }
    std::vector<std::uint32_t> name_string_offsets;
    for (const auto& item : named_exports) {
        name_string_offsets.push_back(static_cast<std::uint32_t>(edata.size()));
        append_string(edata, item.name);
    }
    if (edata.size() > std::numeric_limits<std::uint32_t>::max()) {
        add_diagnostic(result, "XPE64DLL0018", "Export directory exceeds PE RVA range.");
        return result;
    }

    SectionBuild edata_section;
    edata_section.info.name = ".edata";
    edata_section.info.virtual_size = static_cast<std::uint32_t>(edata.size());
    edata_section.info.characteristics = kReadOnlyCharacteristics;
    edata_section.raw = edata;
    std::uint64_t edata_va64 = 0U;
    if (!checked_align(next_rva, spec.section_alignment, edata_va64) || !fits_u32(edata_va64)) {
        add_diagnostic(result, "XPE64DLL0019", "Export directory RVA exceeds PE range.");
        return result;
    }
    edata_section.info.virtual_address = static_cast<std::uint32_t>(edata_va64);
    const std::uint32_t edata_va = edata_section.info.virtual_address;

    // Fill the EAT. Empty ordinal slots remain zero, as defined by PE/COFF.
    for (const auto& pair : bindings) {
        const std::size_t slot = static_cast<std::size_t>(pair.first - ordinal_base);
        std::uint32_t target_rva = 0U;
        if (pair.second.forwarded) {
            const std::uint64_t value = static_cast<std::uint64_t>(edata_va) + forwarder_string_offsets.at(pair.first);
            if (!fits_u32(value)) {
                add_diagnostic(result, "XPE64DLL0020", "Forwarder RVA exceeds PE range.");
                return result;
            }
            target_rva = static_cast<std::uint32_t>(value);
        } else if (!section_rva(spec, section_infos, pair.second.section, pair.second.offset, target_rva)) {
            add_diagnostic(result, "XPE64DLL0021", "Unable to map export target to a section RVA.");
            return result;
        }
        put32(edata, eat_offset + slot * 4U, target_rva);
    }
    for (std::size_t index = 0U; index < named_exports.size(); ++index) {
        const std::uint64_t name_rva = static_cast<std::uint64_t>(edata_va) + name_string_offsets[index];
        if (!fits_u32(name_rva)) {
            add_diagnostic(result, "XPE64DLL0022", "Export name RVA exceeds PE range.");
            return result;
        }
        put32(edata, name_ptr_offset + index * 4U, static_cast<std::uint32_t>(name_rva));
        put16(edata, ordinal_table_offset + index * 2U,
              static_cast<std::uint16_t>(named_exports[index].ordinal - ordinal_base));
    }
    put32(edata, 12U, edata_va + dll_name_offset); // Name
    put32(edata, 16U, ordinal_base);               // Base
    put32(edata, 20U, function_count);             // NumberOfFunctions
    put32(edata, 24U, static_cast<std::uint32_t>(named_count)); // NumberOfNames
    put32(edata, 28U, edata_va + eat_offset);      // AddressOfFunctions
    put32(edata, 32U, edata_va + name_ptr_offset);// AddressOfNames
    put32(edata, 36U, edata_va + ordinal_table_offset); // AddressOfNameOrdinals
    const std::uint32_t export_directory_size = static_cast<std::uint32_t>(edata.size());
    edata_section.raw = std::move(edata);
    sections.push_back(std::move(edata_section));

    std::vector<std::uint32_t> iat_rvas;
    std::uint32_t import_directory_rva = 0U;
    std::uint32_t import_directory_size = 0U;
    std::uint32_t iat_rva = 0U;
    std::uint32_t iat_size = 0U;
    if (!spec.imports.empty()) {
        std::uint64_t idata_va64 = 0U;
        const auto& last_before_idata = sections.back().info;
        if (!checked_align(static_cast<std::uint64_t>(last_before_idata.virtual_address) +
                           last_before_idata.virtual_size, spec.section_alignment, idata_va64) ||
            !fits_u32(idata_va64)) {
            add_diagnostic(result, "XPE64DLL0041", "Import section RVA exceeds PE32+ range.");
            return result;
        }
        auto idata = build_import_section(spec, static_cast<std::uint32_t>(idata_va64), result.image, iat_rvas);
        if (idata.empty() || !fits_u32(static_cast<std::uint64_t>(idata_va64) + idata.size())) {
            add_diagnostic(result, "XPE64DLL0041", "Import Directory, thunks or names exceed PE32+ RVA range.");
            return result;
        }
        SectionBuild idata_section;
        idata_section.info.name = ".idata";
        idata_section.info.virtual_address = static_cast<std::uint32_t>(idata_va64);
        idata_section.info.virtual_size = static_cast<std::uint32_t>(idata.size());
        idata_section.info.characteristics = kReadWriteCharacteristics;
        idata_section.raw = std::move(idata);
        import_directory_rva = result.image.import_directory_rva;
        import_directory_size = result.image.import_directory_size;
        iat_rva = result.image.iat_rva;
        iat_size = result.image.iat_size;
        sections.push_back(std::move(idata_section));

        auto text_section_to_patch = std::find_if(sections.begin(), sections.end(), [](const SectionBuild& item) {
            return item.info.name == ".text";
        });
        if (text_section_to_patch == sections.end()) {
            add_diagnostic(result, "XPE64DLL0041", "IAT call-site patching requires a .text section.");
            return result;
        }
        for (const auto& call : spec.iat_call_sites) {
            const auto import_it = find_import(call);
            const std::size_t import_index = static_cast<std::size_t>(import_it - spec.imports.begin());
            const std::int64_t next_instruction_rva = static_cast<std::int64_t>(text_section_to_patch->info.virtual_address) +
                static_cast<std::int64_t>(call.displacement_offset) + 4;
            const std::int64_t displacement64 = static_cast<std::int64_t>(iat_rvas[import_index]) - next_instruction_rva;
            if (!fits_i32(displacement64)) {
                add_diagnostic(result, "XPE64DLL0041", "RIP-relative IAT displacement exceeds signed 32-bit range.");
                return result;
            }
            const std::int32_t displacement = static_cast<std::int32_t>(displacement64);
            put32(text_section_to_patch->raw, call.displacement_offset,
                  static_cast<std::uint32_t>(displacement));
        }
    }

    std::uint32_t relocation_directory_rva = 0U;
    std::uint32_t relocation_directory_size = 0U;
    const std::size_t total_relocation_count = spec.absolute_relocations.size() + tls_relocation_count;
    if (total_relocation_count > kMaximumRelocations) {
        add_diagnostic(result, "XPE64DLL0028", "Combined user and TLS DIR64 relocation count exceeds the 65536-entry safety limit.");
        return result;
    }
    if (total_relocation_count != 0U) {
        std::map<std::uint32_t, std::vector<std::uint16_t>> grouped_relocations;
        const auto data_info = std::find_if(section_infos.begin(), section_infos.end(), [](const Pe64DllSectionInfo& item) {
            return item.name == ".data";
        });
        if (data_info == section_infos.end()) {
            add_diagnostic(result, "XPE64DLL0029", "DIR64 relocations require an initialized .data section.");
            return result;
        }
        for (const auto& relocation : spec.absolute_relocations) {
            const std::uint64_t destination_rva64 = static_cast<std::uint64_t>(data_info->virtual_address) +
                relocation.destination_offset;
            if (!fits_u32(destination_rva64)) {
                add_diagnostic(result, "XPE64DLL0033", "DIR64 destination RVA exceeds the PE32+ image range.");
                return result;
            }
            const std::uint32_t destination_rva = static_cast<std::uint32_t>(destination_rva64);
            const std::uint32_t page_rva = destination_rva & ~0xfffU;
            const std::uint16_t page_offset = static_cast<std::uint16_t>(destination_rva & 0x0fffU);
            grouped_relocations[page_rva].push_back(static_cast<std::uint16_t>((kRelocDir64 << 12U) | page_offset));
        }
        if (spec.tls.enabled) {
            const auto tls_data_info = std::find_if(section_infos.begin(), section_infos.end(), [](const Pe64DllSectionInfo& item) {
                return item.name == ".data";
            });
            if (tls_data_info == section_infos.end()) {
                add_diagnostic(result, "XPE64DLL0058", "TLS DIR64 entries require a writable .data section.");
                return result;
            }
            std::vector<std::size_t> tls_pointer_offsets = {
                tls_directory_offset, tls_directory_offset + 8U, tls_directory_offset + 16U
            };
            if (!spec.tls.callbacks.empty()) {
                tls_pointer_offsets.push_back(tls_directory_offset + 24U);
                for (std::size_t index = 0U; index < spec.tls.callbacks.size(); ++index)
                    tls_pointer_offsets.push_back(tls_callbacks_offset + index * 8U);
            }
            for (const std::size_t offset : tls_pointer_offsets) {
                const std::uint64_t destination_rva64 = static_cast<std::uint64_t>(tls_data_info->virtual_address) + offset;
                if (!fits_u32(destination_rva64) || (destination_rva64 & 7U) != 0U) {
                    add_diagnostic(result, "XPE64DLL0058", "TLS DIR64 destination is unaligned or exceeds the PE32+ RVA range.");
                    return result;
                }
                const std::uint32_t destination_rva = static_cast<std::uint32_t>(destination_rva64);
                const std::uint32_t page_rva = destination_rva & ~0xfffU;
                const std::uint16_t page_offset = static_cast<std::uint16_t>(destination_rva & 0x0fffU);
                grouped_relocations[page_rva].push_back(static_cast<std::uint16_t>((kRelocDir64 << 12U) | page_offset));
            }
        }
        std::vector<std::uint8_t> relocation_bytes;
        for (auto& group : grouped_relocations) {
            auto& entries = group.second;
            std::sort(entries.begin(), entries.end());
            const std::size_t block_start = relocation_bytes.size();
            append32(relocation_bytes, group.first);
            append32(relocation_bytes, 0U);
            for (const std::uint16_t entry : entries) append16(relocation_bytes, entry);
            if ((entries.size() & 1U) != 0U) append16(relocation_bytes, static_cast<std::uint16_t>(kRelocAbsolute << 12U));
            const std::size_t block_size = relocation_bytes.size() - block_start;
            if (!fits_u32(block_size)) {
                add_diagnostic(result, "XPE64DLL0033", "Base-relocation block exceeds the PE32+ size range.");
                return result;
            }
            put32(relocation_bytes, block_start + 4U, static_cast<std::uint32_t>(block_size));
        }
        std::uint64_t relocation_va64 = 0U;
        const auto& last_before_reloc = sections.back().info;
        if (!checked_align(static_cast<std::uint64_t>(last_before_reloc.virtual_address) +
                           last_before_reloc.virtual_size, spec.section_alignment, relocation_va64) ||
            !fits_u32(relocation_va64)) {
            add_diagnostic(result, "XPE64DLL0033", "Base-relocation section RVA exceeds the PE32+ range.");
            return result;
        }
        SectionBuild relocation_section;
        relocation_section.info.name = ".reloc";
        relocation_section.info.virtual_address = static_cast<std::uint32_t>(relocation_va64);
        relocation_section.info.virtual_size = static_cast<std::uint32_t>(relocation_bytes.size());
        relocation_section.info.characteristics = kRelocReadCharacteristics;
        relocation_section.raw = std::move(relocation_bytes);
        relocation_directory_rva = relocation_section.info.virtual_address;
        relocation_directory_size = relocation_section.info.virtual_size;
        sections.push_back(std::move(relocation_section));
    }

    std::uint32_t exception_directory_rva = 0U;
    std::uint32_t exception_directory_size = 0U;
    if (!sorted_runtime_functions.empty()) {
        if (xdata_bytes.empty() || xdata_bytes.size() > kMaximumUnwindInfoBytes ||
            sorted_runtime_functions.size() > std::numeric_limits<std::uint32_t>::max() / 12U) {
            add_diagnostic(result, "XPE64DLL0066", "Generated .xdata or .pdata exceeds its configured safety limit.");
            return result;
        }
        const auto& last_before_pdata = sections.back().info;
        std::uint64_t pdata_rva64 = 0U;
        if (!checked_align(static_cast<std::uint64_t>(last_before_pdata.virtual_address) +
                           last_before_pdata.virtual_size, spec.section_alignment, pdata_rva64) || !fits_u32(pdata_rva64)) {
            add_diagnostic(result, "XPE64DLL0067", "Exception Directory .pdata RVA exceeds the PE32+ range.");
            return result;
        }
        const std::uint64_t pdata_size64 = static_cast<std::uint64_t>(sorted_runtime_functions.size()) * 12U;
        if (!fits_u32(pdata_size64)) {
            add_diagnostic(result, "XPE64DLL0066", "Runtime function table size exceeds the PE32+ range.");
            return result;
        }
        std::uint64_t xdata_rva64 = 0U;
        if (!checked_align(pdata_rva64 + pdata_size64, spec.section_alignment, xdata_rva64) || !fits_u32(xdata_rva64)) {
            add_diagnostic(result, "XPE64DLL0067", "UNWIND_INFO .xdata RVA exceeds the PE32+ range.");
            return result;
        }
        const auto text_info = std::find_if(sections.begin(), sections.end(), [](const SectionBuild& item) {
            return item.info.name == ".text";
        });
        if (text_info == sections.end()) {
            add_diagnostic(result, "XPE64DLL0067", "Runtime function records require a .text section.");
            return result;
        }
        std::vector<std::uint8_t> pdata_bytes;
        pdata_bytes.reserve(static_cast<std::size_t>(pdata_size64));
        for (std::size_t index = 0U; index < sorted_runtime_functions.size(); ++index) {
            const auto& function = *sorted_runtime_functions[index];
            const std::uint64_t begin_rva64 = static_cast<std::uint64_t>(text_info->info.virtual_address) + function.begin_offset;
            const std::uint64_t end_rva64 = static_cast<std::uint64_t>(text_info->info.virtual_address) + function.end_offset;
            const std::uint64_t unwind_rva64 = xdata_rva64 + unwind_record_offsets[index];
            if (!fits_u32(begin_rva64) || !fits_u32(end_rva64) || !fits_u32(unwind_rva64) ||
                (unwind_rva64 & 3U) != 0U) {
                add_diagnostic(result, "XPE64DLL0067", "RUNTIME_FUNCTION or UNWIND_INFO RVA exceeds the PE32+ range.");
                return result;
            }
            append32(pdata_bytes, static_cast<std::uint32_t>(begin_rva64));
            append32(pdata_bytes, static_cast<std::uint32_t>(end_rva64));
            append32(pdata_bytes, static_cast<std::uint32_t>(unwind_rva64));

            const std::size_t record = unwind_record_offsets[index];
            const std::size_t slots = xdata_bytes[record + 2U];
            const std::size_t tail = record + 4U + ((slots + 1U) & ~std::size_t{1U}) * 2U;
            if ((function.flags & 4U) != 0U) {
                const auto target = std::find_if(sorted_runtime_functions.begin(), sorted_runtime_functions.end(),
                    [&function, &spec](const Pe64DllRuntimeFunction* candidate) {
                        return candidate == &spec.runtime_functions[function.chained_function_index];
                    });
                if (target == sorted_runtime_functions.end()) {
                    add_diagnostic(result, "XPE64DLL0068", "CHAININFO target is absent from the runtime-function table.");
                    return result;
                }
                const std::size_t target_index = static_cast<std::size_t>(target - sorted_runtime_functions.begin());
                const auto& chained = **target;
                put32(xdata_bytes, tail, text_info->info.virtual_address + static_cast<std::uint32_t>(chained.begin_offset));
                put32(xdata_bytes, tail + 4U, text_info->info.virtual_address + static_cast<std::uint32_t>(chained.end_offset));
                put32(xdata_bytes, tail + 8U, static_cast<std::uint32_t>(xdata_rva64 + unwind_record_offsets[target_index]));
            } else if ((function.flags & 3U) != 0U) {
                put32(xdata_bytes, tail, text_info->info.virtual_address + static_cast<std::uint32_t>(function.handler_offset));
            }
        }
        SectionBuild pdata_section;
        pdata_section.info.name = ".pdata";
        pdata_section.info.virtual_address = static_cast<std::uint32_t>(pdata_rva64);
        pdata_section.info.virtual_size = static_cast<std::uint32_t>(pdata_bytes.size());
        pdata_section.info.characteristics = kReadOnlyCharacteristics;
        pdata_section.raw = std::move(pdata_bytes);
        sections.push_back(std::move(pdata_section));

        SectionBuild xdata_section;
        xdata_section.info.name = ".xdata";
        xdata_section.info.virtual_address = static_cast<std::uint32_t>(xdata_rva64);
        xdata_section.info.virtual_size = static_cast<std::uint32_t>(xdata_bytes.size());
        xdata_section.info.characteristics = kReadOnlyCharacteristics;
        xdata_section.raw = std::move(xdata_bytes);
        sections.push_back(std::move(xdata_section));
        exception_directory_rva = static_cast<std::uint32_t>(pdata_rva64);
        exception_directory_size = static_cast<std::uint32_t>(pdata_size64);
    }

    // Assign raw file offsets and final sizes.
    std::uint64_t next_raw = size_of_headers64;
    std::uint64_t size_of_code = 0U;
    std::uint64_t size_of_initialized_data = 0U;
    for (auto& section : sections) {
        std::uint64_t raw_size = 0U;
        if (!checked_align(section.raw.size(), spec.file_alignment, raw_size) || !fits_u32(raw_size)) {
            add_diagnostic(result, "XPE64DLL0023", "Aligned section raw size exceeds PE range.");
            return result;
        }
        std::uint64_t raw_offset = 0U;
        if (!checked_align(next_raw, spec.file_alignment, raw_offset) || !fits_u32(raw_offset)) {
            add_diagnostic(result, "XPE64DLL0024", "Section raw offset exceeds PE range.");
            return result;
        }
        section.info.raw_offset = static_cast<std::uint32_t>(raw_offset);
        section.info.raw_size = static_cast<std::uint32_t>(raw_size);
        next_raw = raw_offset + raw_size;
        if (section.info.name == ".text") size_of_code += raw_size;
        else size_of_initialized_data += raw_size;
    }
    if (!fits_u32(next_raw) || next_raw > spec.maximum_file_size ||
        !fits_u32(size_of_code) || !fits_u32(size_of_initialized_data)) {
        add_diagnostic(result, "XPE64DLL0025", "Final DLL size exceeds configured or PE limits.");
        return result;
    }
    std::uint64_t size_of_image64 = 0U;
    const auto& last_section = sections.back().info;
    if (!checked_align(static_cast<std::uint64_t>(last_section.virtual_address) + last_section.virtual_size,
                       spec.section_alignment, size_of_image64) || !fits_u32(size_of_image64)) {
        add_diagnostic(result, "XPE64DLL0026", "SizeOfImage exceeds PE range.");
        return result;
    }

    std::vector<std::uint8_t> output(static_cast<std::size_t>(next_raw), 0U);
    // DOS header and PE signature.
    output[0] = 'M'; output[1] = 'Z';
    put32(output, kPeOffsetField, kDosHeaderSize);
    const std::size_t pe_offset = kDosHeaderSize;
    output[pe_offset] = 'P'; output[pe_offset + 1U] = 'E';
    const std::size_t coff = pe_offset + 4U;
    put16(output, coff, kMachineAmd64);
    put16(output, coff + 2U, section_count);
    put32(output, coff + 4U, 0U); // deterministic timestamp
    put32(output, coff + 8U, 0U);
    put16(output, coff + 16U, static_cast<std::uint16_t>(kOptionalHeaderSize));
    const std::uint16_t coff_characteristics = static_cast<std::uint16_t>(kExecutableImage | kLargeAddressAware |
        kDll | (total_relocation_count == 0U ? kRelocationsStripped : 0U));
    put16(output, coff + 18U, coff_characteristics);

    const std::size_t optional = coff + kCoffHeaderSize;
    put16(output, optional, kPe32PlusMagic);
    output[optional + 2U] = 14U; // linker major
    output[optional + 3U] = 0U;
    put32(output, optional + 4U, static_cast<std::uint32_t>(size_of_code));
    put32(output, optional + 8U, static_cast<std::uint32_t>(size_of_initialized_data));
    put32(output, optional + 12U, 0U); // SizeOfUninitializedData
    put32(output, optional + 16U, entry_point_rva); // Optional caller-provided DllMain entry point
    const auto text_section = std::find_if(sections.begin(), sections.end(), [](const SectionBuild& item) {
        return item.info.name == ".text";
    });
    put32(output, optional + 20U, text_section->info.virtual_address);
    put64(output, optional + 24U, spec.image_base);
    put32(output, optional + 32U, spec.section_alignment);
    put32(output, optional + 36U, spec.file_alignment);
    put16(output, optional + 40U, 6U); put16(output, optional + 42U, 0U);
    put16(output, optional + 44U, 0U); put16(output, optional + 46U, 0U);
    put16(output, optional + 48U, 6U); put16(output, optional + 50U, 0U);
    put32(output, optional + 52U, 0U);
    put32(output, optional + 56U, static_cast<std::uint32_t>(size_of_image64));
    put32(output, optional + 60U, static_cast<std::uint32_t>(size_of_headers64));
    put32(output, optional + 64U, 0U); // checksum; loader does not require it for ordinary DLLs
    put16(output, optional + 68U, 3U); // WINDOWS_CUI
    const std::uint16_t dll_characteristics = static_cast<std::uint16_t>(kNxCompat |
        (total_relocation_count == 0U ? 0U : kDynamicBase));
    put16(output, optional + 70U, dll_characteristics);
    put64(output, optional + 72U, 0x100000ULL);
    put64(output, optional + 80U, 0x1000ULL);
    put64(output, optional + 88U, 0x100000ULL);
    put64(output, optional + 96U, 0x1000ULL);
    put32(output, optional + 104U, 0U);
    put32(output, optional + 108U, 16U);
    put32(output, optional + 112U, edata_va);
    put32(output, optional + 116U, export_directory_size);
    if (import_directory_rva != 0U) {
        put32(output, optional + 112U + 1U * 8U, import_directory_rva);
        put32(output, optional + 112U + 1U * 8U + 4U, import_directory_size);
    }
    if (relocation_directory_rva != 0U) {
        put32(output, optional + 112U + 5U * 8U, relocation_directory_rva);
        put32(output, optional + 112U + 5U * 8U + 4U, relocation_directory_size);
    }
    if (iat_rva != 0U) {
        put32(output, optional + 112U + 12U * 8U, iat_rva);
        put32(output, optional + 112U + 12U * 8U + 4U, iat_size);
    }
    if (tls_directory_rva != 0U) {
        put32(output, optional + 112U + 9U * 8U, tls_directory_rva);
        put32(output, optional + 112U + 9U * 8U + 4U, 40U);
    }
    if (exception_directory_rva != 0U) {
        put32(output, optional + 112U + 3U * 8U, exception_directory_rva);
        put32(output, optional + 112U + 3U * 8U + 4U, exception_directory_size);
    }

    const std::size_t section_table = optional + kOptionalHeaderSize;
    for (std::size_t index = 0U; index < sections.size(); ++index) {
        const auto& section = sections[index];
        const std::size_t header = section_table + index * kSectionHeaderSize;
        const std::size_t copy_name = std::min<std::size_t>(8U, section.info.name.size());
        std::copy_n(section.info.name.begin(), copy_name, output.begin() + static_cast<std::ptrdiff_t>(header));
        put32(output, header + 8U, section.info.virtual_size);
        put32(output, header + 12U, section.info.virtual_address);
        put32(output, header + 16U, section.info.raw_size);
        put32(output, header + 20U, section.info.raw_offset);
        put32(output, header + 24U, 0U);
        put32(output, header + 28U, 0U);
        put16(output, header + 32U, 0U);
        put16(output, header + 34U, 0U);
        put32(output, header + 36U, section.info.characteristics);
        std::copy(section.raw.begin(), section.raw.end(),
                  output.begin() + static_cast<std::ptrdiff_t>(section.info.raw_offset));
    }

    result.image.bytes = std::move(output);
    result.image.image_base = spec.image_base;
    result.image.size_of_image = static_cast<std::uint32_t>(size_of_image64);
    result.image.size_of_headers = static_cast<std::uint32_t>(size_of_headers64);
    result.image.entry_point_rva = entry_point_rva;
    result.image.export_directory_rva = edata_va;
    result.image.export_directory_size = export_directory_size;
    result.image.relocation_directory_rva = relocation_directory_rva;
    result.image.relocation_directory_size = relocation_directory_size;
    result.image.base_relocation_count = total_relocation_count;
    result.image.has_dynamic_base = total_relocation_count != 0U;
    result.image.export_name_count = named_count;
    result.image.export_ordinal_count = bindings.size();
    result.image.import_directory_rva = import_directory_rva;
    result.image.import_directory_size = import_directory_size;
    result.image.iat_rva = iat_rva;
    result.image.iat_size = iat_size;
    result.image.tls_directory_rva = tls_directory_rva;
    result.image.tls_directory_size = tls_directory_rva == 0U ? 0U : 40U;
    result.image.tls_index_rva = tls_index_rva;
    result.image.tls_template_rva = tls_template_rva;
    result.image.tls_template_size = spec.tls.enabled ? static_cast<std::uint32_t>(spec.tls.template_data.size()) : 0U;
    result.image.tls_zero_fill_size = spec.tls.enabled ? spec.tls.zero_fill_size : 0U;
    result.image.tls_callback_count = spec.tls.enabled ? spec.tls.callbacks.size() : 0U;
    result.image.exception_directory_rva = exception_directory_rva;
    result.image.exception_directory_size = exception_directory_size;
    result.image.runtime_function_count = sorted_runtime_functions.size();
    result.image.unwind_info_size = 0U;
    if (exception_directory_rva != 0U) {
        const auto unwind_section = std::find_if(sections.begin(), sections.end(), [](const SectionBuild& item) {
            return item.info.name == ".xdata";
        });
        if (unwind_section != sections.end()) result.image.unwind_info_size = unwind_section->info.virtual_size;
    }
    for (const auto& section : sections) result.image.sections.push_back(section.info);
    return result;
}

} // namespace xfrontend
