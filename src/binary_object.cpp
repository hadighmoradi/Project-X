// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/binary_object.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace xfrontend {
namespace {

void diag(std::vector<Diagnostic>& out, const char* code, const std::string& message) {
    out.push_back(Diagnostic{code, message, {}});
}

bool align_up(std::size_t value, std::size_t alignment, std::size_t& result) {
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) return false;
    const std::size_t mask = alignment - 1U;
    if (value > std::numeric_limits<std::size_t>::max() - mask) return false;
    result = (value + mask) & ~mask;
    return true;
}

void put_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xffU);
    out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}
void put_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}
void put_u64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i) out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}
void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    const auto at = out.size(); out.resize(at + 2U); put_u16(out, at, value);
}
void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto at = out.size(); out.resize(at + 4U); put_u32(out, at, value);
}
void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    const auto at = out.size(); out.resize(at + 8U); put_u64(out, at, value);
}

bool get_u16(const std::vector<std::uint8_t>& in, std::size_t at, std::uint16_t& value) {
    if (at > in.size() || in.size() - at < 2U) return false;
    value = static_cast<std::uint16_t>(in[at]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[at + 1U]) << 8U);
    return true;
}
bool get_u32(const std::vector<std::uint8_t>& in, std::size_t at, std::uint32_t& value) {
    if (at > in.size() || in.size() - at < 4U) return false;
    value = 0U;
    for (unsigned i = 0; i < 4U; ++i) value |= static_cast<std::uint32_t>(in[at + i]) << (i * 8U);
    return true;
}
bool get_u64(const std::vector<std::uint8_t>& in, std::size_t at, std::uint64_t& value) {
    if (at > in.size() || in.size() - at < 8U) return false;
    value = 0U;
    for (unsigned i = 0; i < 8U; ++i) value |= static_cast<std::uint64_t>(in[at + i]) << (i * 8U);
    return true;
}

bool range_ok(std::size_t total, std::uint64_t offset, std::uint64_t length) {
    if (offset > total || length > total - static_cast<std::size_t>(offset)) return false;
    return true;
}

std::uint32_t elf_reloc_type(ObjectRelocationKind kind) {
    switch (kind) {
    case ObjectRelocationKind::Absolute64: return 1U;       // R_X86_64_64
    case ObjectRelocationKind::PcRelative32: return 2U;     // R_X86_64_PC32
    case ObjectRelocationKind::PltRelative32: return 4U;    // R_X86_64_PLT32
    default: return 0U;
    }
}

std::uint16_t coff_reloc_type(ObjectRelocationKind kind) {
    switch (kind) {
    case ObjectRelocationKind::CoffAddress64: return 0x0001U; // IMAGE_REL_AMD64_ADDR64
    case ObjectRelocationKind::CoffAddr32NB: return 0x0003U; // IMAGE_REL_AMD64_ADDR32NB
    case ObjectRelocationKind::CoffRelative32: return 0x0004U; // IMAGE_REL_AMD64_REL32
    default: return 0U;
    }
}

ObjectRelocationKind elf_kind(std::uint32_t type) {
    switch (type) {
    case 1U: return ObjectRelocationKind::Absolute64;
    case 2U: return ObjectRelocationKind::PcRelative32;
    case 4U: return ObjectRelocationKind::PltRelative32;
    default: return static_cast<ObjectRelocationKind>(255);
    }
}
ObjectRelocationKind coff_kind(std::uint16_t type) {
    switch (type) {
    case 0x0001U: return ObjectRelocationKind::CoffAddress64;
    case 0x0003U: return ObjectRelocationKind::CoffAddr32NB;
    case 0x0004U: return ObjectRelocationKind::CoffRelative32;
    default: return static_cast<ObjectRelocationKind>(255);
    }
}

std::uint32_t elf_section_flags(ObjectSectionKind kind) {
    switch (kind) {
    case ObjectSectionKind::Text: return 0x6U; // SHF_ALLOC | SHF_EXECINSTR
    case ObjectSectionKind::ReadOnlyData: return 0x2U;
    case ObjectSectionKind::Data: return 0x3U;
    case ObjectSectionKind::Bss: return 0x3U;
    case ObjectSectionKind::Metadata: return 0U;
    }
    return 0U;
}

std::uint32_t coff_section_flags(ObjectSectionKind kind, std::size_t alignment, bool comdat) {
    std::uint32_t flags = 0U;
    switch (kind) {
    case ObjectSectionKind::Text: flags = 0x60000020U; break;
    case ObjectSectionKind::ReadOnlyData: flags = 0x40000040U; break;
    case ObjectSectionKind::Data: flags = 0xC0000040U; break;
    case ObjectSectionKind::Bss: flags = 0xC0000080U; break;
    case ObjectSectionKind::Metadata: flags = 0x40000040U; break;
    }
    std::uint32_t alignment_code = 1U;
    std::size_t value = 1U;
    while (value < alignment && alignment_code < 14U) { value <<= 1U; ++alignment_code; }
    if (value == alignment) flags |= (alignment_code << 20U);
    if (comdat) flags |= 0x00001000U; // IMAGE_SCN_LNK_COMDAT
    return flags;
}

std::uint8_t elf_symbol_info(ObjectSymbolBinding binding, ObjectSymbolKind kind) {
    std::uint8_t bind = 1U;
    if (binding == ObjectSymbolBinding::Local) bind = 0U;
    else if (binding == ObjectSymbolBinding::Weak) bind = 2U;
    std::uint8_t type = 0U;
    if (kind == ObjectSymbolKind::Object) type = 1U;
    else if (kind == ObjectSymbolKind::Function) type = 2U;
    else if (kind == ObjectSymbolKind::Section) type = 3U;
    return static_cast<std::uint8_t>((bind << 4U) | type);
}

std::uint16_t coff_symbol_type(ObjectSymbolKind kind) {
    return kind == ObjectSymbolKind::Function ? 0x20U : 0U;
}

void append_string(std::vector<std::uint8_t>& table, const std::string& value) {
    table.insert(table.end(), value.begin(), value.end());
    table.push_back(0U);
}

std::uint32_t string_offset(std::vector<std::uint8_t>& table, std::map<std::string, std::uint32_t>& offsets,
                            const std::string& value) {
    const auto found = offsets.find(value);
    if (found != offsets.end()) return found->second;
    const auto offset = static_cast<std::uint32_t>(table.size());
    offsets.emplace(value, offset);
    append_string(table, value);
    return offset;
}

bool read_c_string(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                   std::size_t limit, std::string& result) {
    if (offset >= limit || limit > bytes.size()) return false;
    std::size_t end = offset;
    while (end < limit && bytes[end] != 0U) ++end;
    if (end == limit) return false;
    result.assign(reinterpret_cast<const char*>(bytes.data() + offset), end - offset);
    return true;
}

struct ElfSectionHeader {
    std::uint32_t name = 0U;
    std::uint32_t type = 0U;
    std::uint64_t flags = 0U;
    std::uint64_t address = 0U;
    std::uint64_t offset = 0U;
    std::uint64_t size = 0U;
    std::uint32_t link = 0U;
    std::uint32_t info = 0U;
    std::uint64_t alignment = 1U;
    std::uint64_t entry_size = 0U;
};

void append_elf_section_header(std::vector<std::uint8_t>& out, const ElfSectionHeader& h) {
    append_u32(out, h.name); append_u32(out, h.type); append_u64(out, h.flags);
    append_u64(out, h.address); append_u64(out, h.offset); append_u64(out, h.size);
    append_u32(out, h.link); append_u32(out, h.info); append_u64(out, h.alignment);
    append_u64(out, h.entry_size);
}

BinaryObjectWriteResult write_elf(const ObjectFile& object) {
    BinaryObjectWriteResult result;
    if (std::any_of(object.sections.begin(), object.sections.end(), [](const ObjectSection& section) { return section.comdat; })) {
        diag(result.diagnostics, "XBIN0012", "COFF COMDAT section flags cannot be serialized in ELF objects"); return result;
    }
    if (object.descriptor.format != ObjectFileFormat::ELF ||
        object.descriptor.architecture != ObjectArchitecture::X86_64) {
        diag(result.diagnostics, "XBIN0001", "N23 ELF writer supports only ELF64 x86-64"); return result;
    }
    const auto validation = validate_object_file(object);
    if (!validation.succeeded()) { result.diagnostics = validation.diagnostics; return result; }

    std::vector<std::uint8_t> out(64U, 0U);
    out[0] = 0x7fU; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
    out[4] = 2U; out[5] = 1U; out[6] = 1U;
    put_u16(out, 16U, 1U); put_u16(out, 18U, 62U); put_u32(out, 20U, 1U);
    put_u16(out, 52U, 64U); put_u16(out, 54U, 0U); put_u16(out, 56U, 0U); put_u16(out, 58U, 64U);

    std::vector<ElfSectionHeader> headers(1U);
    std::vector<std::string> section_names(1U);
    std::map<ObjectSectionId, std::uint32_t> section_indices;
    for (const auto& section : object.sections) {
        ElfSectionHeader h;
        h.type = section.kind == ObjectSectionKind::Bss ? 8U : 1U;
        h.flags = elf_section_flags(section.kind);
        h.alignment = section.alignment;
        const std::size_t index = headers.size();
        if (index > std::numeric_limits<std::uint32_t>::max()) { diag(result.diagnostics, "XBIN0002", "too many ELF sections"); return result; }
        section_indices.emplace(section.id, static_cast<std::uint32_t>(index));
        headers.push_back(h); section_names.push_back(section.name);
    }

    // Symbols are reordered according to ELF's local-before-global rule.
    std::vector<const ObjectSymbol*> ordered_symbols;
    for (const auto& symbol : object.symbols) if (symbol.binding == ObjectSymbolBinding::Local) ordered_symbols.push_back(&symbol);
    const std::size_t first_global = ordered_symbols.size() + 1U;
    for (const auto& symbol : object.symbols) if (symbol.binding != ObjectSymbolBinding::Local) ordered_symbols.push_back(&symbol);
    std::vector<std::uint8_t> strtab(1U, 0U);
    std::map<std::string, std::uint32_t> str_offsets;
    std::map<std::string, std::uint32_t> symbol_indices;
    std::vector<std::uint8_t> symtab(24U, 0U); // mandatory null symbol
    for (std::size_t i = 0; i < ordered_symbols.size(); ++i) {
        const auto& symbol = *ordered_symbols[i];
        const auto name_offset = string_offset(strtab, str_offsets, symbol.name);
        symbol_indices.emplace(symbol.name, static_cast<std::uint32_t>(i + 1U));
        append_u32(symtab, name_offset);
        symtab.push_back(elf_symbol_info(symbol.binding, symbol.kind));
        symtab.push_back(0U);
        std::uint16_t shndx = 0U;
        if (symbol.defined) shndx = static_cast<std::uint16_t>(section_indices.at(symbol.section_id));
        append_u16(symtab, shndx);
        append_u64(symtab, symbol.value);
        append_u64(symtab, symbol.size);
    }

    // Add symbol/string tables and one SHT_RELA section for each section that
    // carries relocations. String-table indexes are resolved after all names exist.
    const std::uint32_t symtab_index = static_cast<std::uint32_t>(headers.size());
    headers.push_back(ElfSectionHeader{}); section_names.push_back(".symtab");
    const std::uint32_t strtab_index = static_cast<std::uint32_t>(headers.size());
    headers.push_back(ElfSectionHeader{}); section_names.push_back(".strtab");
    std::map<ObjectSectionId, std::uint32_t> rela_indices;
    for (const auto& section : object.sections) {
        const bool has_relocations = std::any_of(object.relocations.begin(), object.relocations.end(),
            [&section](const ObjectRelocation& r) { return r.section_id == section.id; });
        if (!has_relocations) continue;
        const auto index = static_cast<std::uint32_t>(headers.size());
        rela_indices.emplace(section.id, index);
        headers.push_back(ElfSectionHeader{});
        section_names.push_back(".rela" + section.name);
    }
    const std::uint32_t shstrtab_index = static_cast<std::uint32_t>(headers.size());
    headers.push_back(ElfSectionHeader{}); section_names.push_back(".shstrtab");

    // Section name table and header name offsets.
    std::vector<std::uint8_t> shstrtab(1U, 0U);
    std::map<std::string, std::uint32_t> shname_offsets;
    for (std::size_t i = 1U; i < headers.size(); ++i) {
        headers[i].name = string_offset(shstrtab, shname_offsets, section_names[i]);
    }

    // Emit section payloads in section-table order. All offsets are file-relative.
    for (std::size_t i = 0; i < object.sections.size(); ++i) {
        const auto& section = object.sections[i];
        auto& header = headers[i + 1U];
        std::size_t aligned = 0U;
        if (!align_up(out.size(), section.alignment, aligned)) { diag(result.diagnostics, "XBIN0003", "ELF section alignment overflow"); return result; }
        out.resize(aligned, 0U);
        header.offset = out.size();
        header.size = section.size;
        if (section.kind != ObjectSectionKind::Bss) out.insert(out.end(), section.contents.begin(), section.contents.end());
    }

    auto emit_payload = [&out](ElfSectionHeader& header, const std::vector<std::uint8_t>& payload, std::size_t alignment) -> bool {
        std::size_t aligned = 0U;
        if (!align_up(out.size(), alignment, aligned)) return false;
        out.resize(aligned, 0U);
        header.offset = out.size(); header.size = payload.size();
        out.insert(out.end(), payload.begin(), payload.end()); return true;
    };

    auto& sym_header = headers[symtab_index];
    sym_header.type = 2U; sym_header.alignment = 8U; sym_header.entry_size = 24U;
    sym_header.link = strtab_index; sym_header.info = static_cast<std::uint32_t>(first_global);
    if (!emit_payload(sym_header, symtab, 8U)) { diag(result.diagnostics, "XBIN0004", "ELF symbol-table alignment overflow"); return result; }
    auto& str_header = headers[strtab_index]; str_header.type = 3U; str_header.alignment = 1U;
    if (!emit_payload(str_header, strtab, 1U)) { diag(result.diagnostics, "XBIN0005", "ELF string-table alignment overflow"); return result; }

    for (const auto& section : object.sections) {
        const auto rela_it = rela_indices.find(section.id);
        if (rela_it == rela_indices.end()) continue;
        std::vector<std::uint8_t> payload;
        for (const auto& relocation : object.relocations) {
            if (relocation.section_id != section.id) continue;
            const auto symbol_it = symbol_indices.find(relocation.target_symbol);
            if (symbol_it == symbol_indices.end()) { diag(result.diagnostics, "XBIN0006", "relocation symbol is not encodable"); return result; }
            const auto type = elf_reloc_type(relocation.kind);
            if (type == 0U) { diag(result.diagnostics, "XBIN0007", "relocation kind is not supported by ELF64 x86-64 writer"); return result; }
            append_u64(payload, relocation.offset);
            append_u64(payload, (static_cast<std::uint64_t>(symbol_it->second) << 32U) | type);
            append_u64(payload, static_cast<std::uint64_t>(relocation.addend));
        }
        auto& h = headers[rela_it->second];
        h.type = 4U; h.alignment = 8U; h.entry_size = 24U; h.link = symtab_index;
        h.info = section_indices.at(section.id);
        if (!emit_payload(h, payload, 8U)) { diag(result.diagnostics, "XBIN0008", "ELF relocation alignment overflow"); return result; }
    }

    auto& shstr_header = headers[shstrtab_index]; shstr_header.type = 3U; shstr_header.alignment = 1U;
    if (!emit_payload(shstr_header, shstrtab, 1U)) { diag(result.diagnostics, "XBIN0009", "ELF section-name table alignment overflow"); return result; }

    std::size_t shoff = 0U;
    if (!align_up(out.size(), 8U, shoff)) { diag(result.diagnostics, "XBIN0010", "ELF section-header alignment overflow"); return result; }
    out.resize(shoff, 0U);
    for (const auto& h : headers) append_elf_section_header(out, h);
    put_u64(out, 40U, shoff);
    if (headers.size() > std::numeric_limits<std::uint16_t>::max()) { diag(result.diagnostics, "XBIN0011", "ELF extended section counts are not supported"); return result; }
    put_u16(out, 60U, static_cast<std::uint16_t>(headers.size()));
    put_u16(out, 62U, static_cast<std::uint16_t>(shstrtab_index));
    result.bytes = std::move(out);
    return result;
}

struct CoffSymbolEncoding {
    const ObjectSymbol* symbol = nullptr;
    std::uint32_t table_index = 0U;
    std::uint8_t auxiliary_count = 0U;
    bool section_definition = false;
    std::uint16_t section_number = 0U;
    std::uint16_t section_relocation_count = 0U;
    std::uint32_t section_length = 0U;
};

BinaryObjectWriteResult write_coff(const ObjectFile& object) {
    BinaryObjectWriteResult result;
    if (object.descriptor.format != ObjectFileFormat::COFF ||
        object.descriptor.architecture != ObjectArchitecture::X86_64) {
        diag(result.diagnostics, "XBIN0020", "N23 COFF writer supports only AMD64 COFF"); return result;
    }
    const auto validation = validate_object_file(object);
    if (!validation.succeeded()) { result.diagnostics = validation.diagnostics; return result; }
    if (object.sections.size() > 0xffffU) { diag(result.diagnostics, "XBIN0021", "COFF section count exceeds 16-bit limit"); return result; }
    for (const auto& section : object.sections) {
        if (section.alignment > 8192U) { diag(result.diagnostics, "XBIN0031", "COFF section alignment above 8192 bytes is not supported"); return result; }
        if (section.size > 0xffffffffU || section.contents.size() > 0xffffffffU) {
            diag(result.diagnostics, "XBIN0032", "COFF section size exceeds the 32-bit object-file limit"); return result;
        }
    }
    for (const auto& symbol : object.symbols) {
        if (symbol.value > 0xffffffffU) { diag(result.diagnostics, "XBIN0033", "COFF symbol value exceeds the 32-bit section-offset limit"); return result; }
    }
    for (const auto& relocation : object.relocations) {
        if (relocation.offset > 0xffffffffU) { diag(result.diagnostics, "XBIN0034", "COFF relocation offset exceeds the 32-bit limit"); return result; }
        if (relocation.kind == ObjectRelocationKind::CoffRelative32 &&
            (relocation.addend < std::numeric_limits<std::int32_t>::min() || relocation.addend > std::numeric_limits<std::int32_t>::max())) {
            diag(result.diagnostics, "XBIN0035", "COFF REL32 implicit addend exceeds signed 32-bit range"); return result;
        }
        if (relocation.kind == ObjectRelocationKind::CoffAddr32NB &&
            (relocation.addend < 0 || static_cast<std::uint64_t>(relocation.addend) > std::numeric_limits<std::uint32_t>::max())) {
            diag(result.diagnostics, "XBIN0036", "COFF ADDR32NB implicit addend exceeds unsigned 32-bit range"); return result;
        }
    }

    std::vector<std::uint8_t> out(20U + object.sections.size() * 40U, 0U);
    put_u16(out, 0U, 0x8664U); put_u16(out, 2U, static_cast<std::uint16_t>(object.sections.size()));
    std::vector<std::uint8_t> string_table(4U, 0U);
    std::map<std::string, std::uint32_t> string_offsets;
    std::vector<std::uint32_t> section_string_offsets(object.sections.size(), 0U);
    for (std::size_t i = 0; i < object.sections.size(); ++i) {
        const auto& name = object.sections[i].name;
        if (name.size() > 8U) section_string_offsets[i] = string_offset(string_table, string_offsets, name);
    }

    std::vector<const ObjectSymbol*> ordered_local;
    std::vector<const ObjectSymbol*> ordered_external;
    for (const auto& symbol : object.symbols) {
        (symbol.binding == ObjectSymbolBinding::Local ? ordered_local : ordered_external).push_back(&symbol);
    }

    std::vector<ObjectSymbol> section_symbols;
    section_symbols.reserve(object.sections.size());
    std::map<ObjectSectionId, std::size_t> section_definition_positions;
    for (const auto& section : object.sections) {
        if (!section.comdat) continue;
        ObjectSymbol symbol;
        symbol.name = section.name;
        symbol.signature_key = "coff-section-definition";
        symbol.binding = ObjectSymbolBinding::Local;
        symbol.kind = ObjectSymbolKind::Section;
        symbol.defined = true;
        symbol.section_id = section.id;
        symbol.size = section.size;
        section_definition_positions.emplace(section.id, section_symbols.size());
        section_symbols.push_back(std::move(symbol));
    }

    std::vector<CoffSymbolEncoding> encodings;
    std::map<std::string, std::uint32_t> symbol_indices;
    std::map<ObjectSectionId, std::uint32_t> section_symbol_indices;
    std::set<const ObjectSymbol*> emitted_user_symbols;
    std::uint32_t table_index = 0U;
    const auto append_user_symbol = [&encodings, &symbol_indices, &table_index](const ObjectSymbol* symbol) -> bool {
        CoffSymbolEncoding encoding;
        encoding.symbol = symbol;
        encoding.table_index = table_index;
        if (symbol->binding == ObjectSymbolBinding::Weak) encoding.auxiliary_count = 1U;
        if (!symbol_indices.emplace(symbol->name, table_index).second) return false;
        encodings.push_back(encoding);
        table_index += 1U + encoding.auxiliary_count;
        return true;
    };

    // COFF COMDAT records are a sequence, not an unordered collection:
    //   section-definition symbol, its auxiliary section-definition record,
    //   and (for non-associative COMDATs) the section's COMDAT leader symbol.
    // LINK.EXE expects the leader immediately after the section symbol/aux pair.
    // Associative COMDATs (.xdata$ / .pdata$) have Selection=ASSOCIATIVE and,
    // like clang-cl output, carry only the section-definition pair; their
    // lifetime is keyed by the earlier primary COMDAT section.
    for (std::size_t section_position = 0U; section_position < object.sections.size(); ++section_position) {
        const auto& section = object.sections[section_position];
        if (!section.comdat) continue;

        const auto definition_position = section_definition_positions.find(section.id);
        if (definition_position == section_definition_positions.end() ||
            definition_position->second >= section_symbols.size()) {
            diag(result.diagnostics, "XBIN0037", "COMDAT section definition has no matching section");
            return result;
        }
        const auto& section_symbol = section_symbols[definition_position->second];
        CoffSymbolEncoding definition;
        definition.symbol = &section_symbol;
        definition.table_index = table_index;
        definition.auxiliary_count = 1U;
        definition.section_definition = true;
        definition.section_number = static_cast<std::uint16_t>(section_position + 1U);
        definition.section_length = static_cast<std::uint32_t>(section.size);
        if (!symbol_indices.emplace(section.name, table_index).second) {
            diag(result.diagnostics, "XBIN0039", "COFF section-definition symbol name collides with another symbol");
            return result;
        }
        section_symbol_indices.emplace(section.id, table_index);
        encodings.push_back(definition);
        table_index += 2U;

        if (section.associative_to_section_id == InvalidObjectSectionId) {
            // A primary COMDAT must be followed by its key/leader symbol.
            // Prefer a non-local function symbol (the normal .text case),
            // then any non-local definition, and finally a local definition.
            auto leader = std::find_if(object.symbols.begin(), object.symbols.end(),
                [&section](const ObjectSymbol& symbol) {
                    return symbol.defined && symbol.section_id == section.id &&
                           symbol.kind == ObjectSymbolKind::Function &&
                           symbol.binding != ObjectSymbolBinding::Local;
                });
            if (leader == object.symbols.end()) {
                leader = std::find_if(object.symbols.begin(), object.symbols.end(),
                    [&section](const ObjectSymbol& symbol) {
                        return symbol.defined && symbol.section_id == section.id &&
                               symbol.kind != ObjectSymbolKind::Section &&
                               symbol.binding != ObjectSymbolBinding::Local;
                    });
            }
            if (leader == object.symbols.end()) {
                leader = std::find_if(object.symbols.begin(), object.symbols.end(),
                    [&section](const ObjectSymbol& symbol) {
                        return symbol.defined && symbol.section_id == section.id &&
                               symbol.kind != ObjectSymbolKind::Section;
                    });
            }
            if (leader == object.symbols.end()) {
                diag(result.diagnostics, "XBIN0042", "primary COFF COMDAT section has no defined leader symbol");
                return result;
            }
            if (!append_user_symbol(&*leader)) {
                diag(result.diagnostics, "XBIN0043", "COFF COMDAT leader symbol name collides with an existing symbol");
                return result;
            }
            emitted_user_symbols.insert(&*leader);
        }
    }

    // Remaining local symbols precede remaining external symbols as required
    // by the COFF symbol-table convention. COMDAT leaders have already been
    // emitted adjacent to their section-definition records and are skipped.
    for (const auto* symbol : ordered_local) {
        if (emitted_user_symbols.find(symbol) != emitted_user_symbols.end()) continue;
        if (!append_user_symbol(symbol)) {
            diag(result.diagnostics, "XBIN0044", "duplicate COFF local symbol name");
            return result;
        }
    }
    for (const auto* symbol : ordered_external) {
        if (emitted_user_symbols.find(symbol) != emitted_user_symbols.end()) continue;
        if (!append_user_symbol(symbol)) {
            diag(result.diagnostics, "XBIN0045", "duplicate COFF external symbol name");
            return result;
        }
    }

    std::vector<std::vector<std::uint8_t>> relocation_records(object.sections.size());
    std::vector<std::uint16_t> relocation_counts(object.sections.size(), 0U);
    for (const auto& relocation : object.relocations) {
        const auto section_it = std::find_if(object.sections.begin(), object.sections.end(), [&relocation](const ObjectSection& s) { return s.id == relocation.section_id; });
        if (section_it == object.sections.end()) { diag(result.diagnostics, "XBIN0022", "COFF relocation source section is missing"); return result; }
        const std::size_t section_index = static_cast<std::size_t>(section_it - object.sections.begin());
        const auto symbol_it = symbol_indices.find(relocation.target_symbol);
        const auto type = coff_reloc_type(relocation.kind);
        if (symbol_it == symbol_indices.end() || type == 0U) { diag(result.diagnostics, "XBIN0023", "COFF relocation target or kind cannot be encoded"); return result; }
        std::uint32_t relocation_symbol_index = symbol_it->second;
        if (relocation.kind == ObjectRelocationKind::CoffAddr32NB) {
            const auto target = std::find_if(object.symbols.begin(), object.symbols.end(),
                [&relocation](const ObjectSymbol& symbol) { return symbol.name == relocation.target_symbol; });
            if (target != object.symbols.end() && target->defined) {
                const auto section_symbol = section_symbol_indices.find(target->section_id);
                if (section_symbol != section_symbol_indices.end())
                    relocation_symbol_index = section_symbol->second;
            }
        }
        if (relocation_counts[section_index] == 0xffffU) { diag(result.diagnostics, "XBIN0024", "COFF relocation count exceeds 16-bit limit"); return result; }
        append_u32(relocation_records[section_index], static_cast<std::uint32_t>(relocation.offset));
        append_u32(relocation_records[section_index], relocation_symbol_index);
        append_u16(relocation_records[section_index], type);
        ++relocation_counts[section_index];
    }

    for (auto& encoding : encodings) {
        if (!encoding.section_definition) continue;
        const std::size_t section_position = static_cast<std::size_t>(encoding.section_number - 1U);
        encoding.section_relocation_count = relocation_counts[section_position];
    }

    std::vector<std::uint32_t> raw_offsets(object.sections.size(), 0U);
    std::vector<std::uint32_t> reloc_offsets(object.sections.size(), 0U);
    for (std::size_t i = 0; i < object.sections.size(); ++i) {
        const auto& section = object.sections[i];
        auto header = 20U + i * 40U;
        if (section.name.size() <= 8U) std::copy(section.name.begin(), section.name.end(), out.begin() + static_cast<std::ptrdiff_t>(header));
        else {
            const std::string long_name = "/" + std::to_string(section_string_offsets[i]);
            if (long_name.size() > 8U) { diag(result.diagnostics, "XBIN0025", "COFF section string-table offset does not fit section name field"); return result; }
            std::copy(long_name.begin(), long_name.end(), out.begin() + static_cast<std::ptrdiff_t>(header));
        }
        put_u32(out, header + 8U, section.kind == ObjectSectionKind::Bss ? static_cast<std::uint32_t>(section.size) : 0U);
        put_u32(out, header + 16U, section.kind == ObjectSectionKind::Bss ? 0U : static_cast<std::uint32_t>(section.contents.size()));
        put_u32(out, header + 36U, coff_section_flags(section.kind, section.alignment, section.comdat));
        if (section.kind != ObjectSectionKind::Bss && !section.contents.empty()) {
            std::size_t aligned = 0U;
            if (!align_up(out.size(), std::max<std::size_t>(1U, section.alignment), aligned) || aligned > 0xffffffffU) {
                diag(result.diagnostics, "XBIN0026", "COFF raw-data alignment overflow"); return result;
            }
            out.resize(aligned, 0U); raw_offsets[i] = static_cast<std::uint32_t>(out.size());
            std::vector<std::uint8_t> raw = section.contents;
            for (const auto& relocation : object.relocations) {
                if (relocation.section_id != section.id) continue;
                if (relocation.kind == ObjectRelocationKind::CoffRelative32) {
                    put_u32(raw, relocation.offset, static_cast<std::uint32_t>(static_cast<std::int32_t>(relocation.addend)));
                } else if (relocation.kind == ObjectRelocationKind::CoffAddr32NB) {
                    std::int64_t effective_addend = relocation.addend;
                    const auto target = std::find_if(object.symbols.begin(), object.symbols.end(),
                        [&relocation](const ObjectSymbol& symbol) { return symbol.name == relocation.target_symbol; });
                    if (target != object.symbols.end() && target->defined) {
                        if (target->value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                            diag(result.diagnostics, "XBIN0040", "COFF ADDR32NB symbol offset exceeds signed addend range");
                            return result;
                        }
                        const auto symbol_value = static_cast<std::int64_t>(target->value);
                        if (effective_addend > std::numeric_limits<std::int64_t>::max() - symbol_value) {
                            diag(result.diagnostics, "XBIN0040", "COFF ADDR32NB section-symbol addend overflows");
                            return result;
                        }
                        effective_addend += symbol_value;
                    }
                    if (effective_addend < 0 || effective_addend > std::numeric_limits<std::uint32_t>::max()) {
                        diag(result.diagnostics, "XBIN0041", "COFF ADDR32NB addend is outside unsigned 32-bit range");
                        return result;
                    }
                    put_u32(raw, relocation.offset, static_cast<std::uint32_t>(effective_addend));
                } else if (relocation.kind == ObjectRelocationKind::CoffAddress64) {
                    put_u64(raw, relocation.offset, static_cast<std::uint64_t>(relocation.addend));
                }
            }
            out.insert(out.end(), raw.begin(), raw.end());
        }
        if (!relocation_records[i].empty()) {
            if (out.size() > 0xffffffffU) { diag(result.diagnostics, "XBIN0027", "COFF relocation offset exceeds 32-bit limit"); return result; }
            reloc_offsets[i] = static_cast<std::uint32_t>(out.size());
            out.insert(out.end(), relocation_records[i].begin(), relocation_records[i].end());
        }
    }

    if (out.size() > 0xffffffffU) { diag(result.diagnostics, "XBIN0028", "COFF symbol-table offset exceeds 32-bit limit"); return result; }
    const std::uint32_t symbol_table_offset = static_cast<std::uint32_t>(out.size());
    for (const auto& encoding : encodings) {
        const auto& symbol = *encoding.symbol;
        const std::size_t start = out.size(); out.resize(start + 18U, 0U);
        if (symbol.name.size() <= 8U) std::copy(symbol.name.begin(), symbol.name.end(), out.begin() + static_cast<std::ptrdiff_t>(start));
        else {
            const auto offset = string_offset(string_table, string_offsets, symbol.name);
            put_u32(out, start, 0U); put_u32(out, start + 4U, offset);
        }
        put_u32(out, start + 8U, static_cast<std::uint32_t>(symbol.value));
        std::uint16_t section_number = 0U;
        if (encoding.section_definition) {
            section_number = encoding.section_number;
        } else if (symbol.defined) {
            const auto it = std::find_if(object.sections.begin(), object.sections.end(), [&symbol](const ObjectSection& s) { return s.id == symbol.section_id; });
            if (it == object.sections.end()) { diag(result.diagnostics, "XBIN0029", "COFF symbol section is missing"); return result; }
            section_number = static_cast<std::uint16_t>((it - object.sections.begin()) + 1);
        }
        put_u16(out, start + 12U, section_number);
        put_u16(out, start + 14U, coff_symbol_type(symbol.kind));
        out[start + 16U] = symbol.binding == ObjectSymbolBinding::Local ? 3U :
                           (symbol.binding == ObjectSymbolBinding::Weak ? 105U : 2U);
        out[start + 17U] = encoding.auxiliary_count;
        if (encoding.auxiliary_count != 0U) {
            const std::size_t aux = out.size(); out.resize(aux + 18U, 0U);
            if (encoding.section_definition) {
                put_u32(out, aux, encoding.section_length);
                put_u16(out, aux + 4U, encoding.section_relocation_count);
                put_u16(out, aux + 6U, 0U);
                put_u32(out, aux + 8U, 0U);
                std::uint16_t associated_section_number = 0U;
                if (encoding.symbol->section_id != InvalidObjectSectionId) {
                    const auto definition = std::find_if(object.sections.begin(), object.sections.end(),
                        [&encoding](const ObjectSection& section) {
                            return section.id == encoding.symbol->section_id;
                        });
                    if (definition != object.sections.end() &&
                        definition->associative_to_section_id != InvalidObjectSectionId) {
                        const auto key = std::find_if(object.sections.begin(), object.sections.end(),
                            [&definition](const ObjectSection& section) {
                                return section.id == definition->associative_to_section_id;
                            });
                        if (key == object.sections.end()) {
                            diag(result.diagnostics, "XBIN0038", "associative COMDAT key section is missing");
                            return result;
                        }
                        associated_section_number = static_cast<std::uint16_t>((key - object.sections.begin()) + 1U);
                    }
                }
                put_u16(out, aux + 12U, associated_section_number);
                out[aux + 14U] = associated_section_number == 0U ? 1U : 5U; // NODUPLICATES / ASSOCIATIVE
            } else {
                put_u32(out, aux, 0U); put_u32(out, aux + 4U, 2U); // weak external: search library
            }
        }
    }
    put_u32(string_table, 0U, static_cast<std::uint32_t>(string_table.size()));
    out.insert(out.end(), string_table.begin(), string_table.end());
    put_u32(out, 8U, symbol_table_offset);
    put_u32(out, 12U, table_index);

    for (std::size_t i = 0; i < object.sections.size(); ++i) {
        const std::size_t header = 20U + i * 40U;
        put_u32(out, header + 20U, raw_offsets[i]);
        put_u32(out, header + 24U, reloc_offsets[i]);
        put_u32(out, header + 28U, 0U);
        put_u16(out, header + 32U, relocation_counts[i]);
        put_u16(out, header + 34U, 0U);
    }
    result.bytes = std::move(out);
    return result;
}

BinaryObjectReadResult read_elf(const std::vector<std::uint8_t>& bytes) {
    BinaryObjectReadResult result;
    const auto fail = [&result](const char* code, const std::string& message) { diag(result.diagnostics, code, message); };
    if (bytes.size() < 64U || bytes[0] != 0x7fU || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        fail("XBIN0100", "input is not a complete ELF header"); return result;
    }
    if (bytes[4] != 2U || bytes[5] != 1U || bytes[6] != 1U) { fail("XBIN0101", "unsupported ELF class, byte order or version"); return result; }
    std::uint16_t type = 0U, machine = 0U, ehsize = 0U, shentsize = 0U, shnum = 0U, shstrndx = 0U;
    std::uint64_t shoff = 0U;
    if (!get_u16(bytes, 16U, type) || !get_u16(bytes, 18U, machine) || !get_u64(bytes, 40U, shoff) ||
        !get_u16(bytes, 52U, ehsize) || !get_u16(bytes, 58U, shentsize) || !get_u16(bytes, 60U, shnum) || !get_u16(bytes, 62U, shstrndx)) {
        fail("XBIN0102", "truncated ELF header"); return result;
    }
    if (type != 1U || machine != 62U || ehsize != 64U || shentsize != 64U || shnum == 0U || shstrndx >= shnum) {
        fail("XBIN0103", "ELF input is not a supported x86-64 relocatable object"); return result;
    }
    if (!range_ok(bytes.size(), shoff, static_cast<std::uint64_t>(shnum) * 64U)) { fail("XBIN0104", "ELF section-header table is out of bounds"); return result; }
    std::vector<ElfSectionHeader> headers(shnum);
    for (std::size_t i = 0; i < shnum; ++i) {
        const std::size_t at = static_cast<std::size_t>(shoff) + i * 64U;
        auto& h = headers[i];
        if (!get_u32(bytes, at, h.name) || !get_u32(bytes, at + 4U, h.type) ||
            !get_u64(bytes, at + 8U, h.flags) || !get_u64(bytes, at + 24U, h.offset) ||
            !get_u64(bytes, at + 32U, h.size) || !get_u32(bytes, at + 40U, h.link) ||
            !get_u32(bytes, at + 44U, h.info) || !get_u64(bytes, at + 48U, h.alignment) ||
            !get_u64(bytes, at + 56U, h.entry_size)) { fail("XBIN0105", "truncated ELF section header"); return result; }
        if (h.type != 8U && h.size != 0U && !range_ok(bytes.size(), h.offset, h.size)) { fail("XBIN0106", "ELF section payload is out of bounds"); return result; }
        if (h.alignment != 0U && (h.alignment & (h.alignment - 1U)) != 0U) { fail("XBIN0107", "ELF section alignment is invalid"); return result; }
    }
    const auto& shstr = headers[shstrndx];
    if (shstr.type != 3U || !range_ok(bytes.size(), shstr.offset, shstr.size)) { fail("XBIN0108", "ELF section-name string table is invalid"); return result; }
    std::vector<std::string> names(shnum);
    for (std::size_t i = 1U; i < shnum; ++i) {
        if (!read_c_string(bytes, static_cast<std::size_t>(shstr.offset) + headers[i].name,
                           static_cast<std::size_t>(shstr.offset + shstr.size), names[i])) {
            fail("XBIN0109", "ELF section name is invalid"); return result;
        }
    }
    result.object_file.object_id = "elf64-read";
    result.object_file.descriptor = make_elf_x86_64_object_target();
    std::vector<std::uint32_t> section_index_to_model(shnum, InvalidObjectSectionId);
    for (std::size_t i = 1U; i < shnum; ++i) {
        const auto& h = headers[i];
        if (h.type != 1U && h.type != 8U) continue;
        ObjectSection section;
        section.id = static_cast<ObjectSectionId>(result.object_file.sections.size() + 1U);
        section.name = names[i]; section.size = static_cast<std::size_t>(h.size);
        section.alignment = static_cast<std::size_t>(std::max<std::uint64_t>(1U, h.alignment));
        if (h.type == 8U) section.kind = ObjectSectionKind::Bss;
        else if ((h.flags & 0x4U) != 0U) section.kind = ObjectSectionKind::Text;
        else if ((h.flags & 0x2U) == 0U) section.kind = ObjectSectionKind::Metadata;
        else if ((h.flags & 0x1U) != 0U) section.kind = ObjectSectionKind::Data;
        else section.kind = ObjectSectionKind::ReadOnlyData;
        if (h.type != 8U) section.contents.assign(bytes.begin() + static_cast<std::ptrdiff_t>(h.offset), bytes.begin() + static_cast<std::ptrdiff_t>(h.offset + h.size));
        section_index_to_model[i] = section.id;
        result.object_file.sections.push_back(std::move(section));
    }

    std::uint32_t symtab_index = 0U;
    for (std::size_t i = 1U; i < shnum; ++i) if (headers[i].type == 2U) { symtab_index = static_cast<std::uint32_t>(i); break; }
    std::vector<std::string> symbol_names;
    if (symtab_index != 0U) {
        const auto& symh = headers[symtab_index];
        if (symh.link >= shnum || headers[symh.link].type != 3U || symh.entry_size != 24U || symh.size % 24U != 0U ||
            !range_ok(bytes.size(), symh.offset, symh.size)) { fail("XBIN0110", "ELF symbol table metadata is invalid"); return result; }
        const auto& strh = headers[symh.link];
        const std::size_t str_end = static_cast<std::size_t>(strh.offset + strh.size);
        const std::size_t count = static_cast<std::size_t>(symh.size / 24U);
        symbol_names.resize(count);
        for (std::size_t i = 1U; i < count; ++i) {
            const std::size_t at = static_cast<std::size_t>(symh.offset) + i * 24U;
            std::uint32_t name_offset = 0U; std::uint16_t shndx = 0U; std::uint64_t value = 0U, size = 0U;
            if (!get_u32(bytes, at, name_offset) || !get_u16(bytes, at + 6U, shndx) ||
                !get_u64(bytes, at + 8U, value) || !get_u64(bytes, at + 16U, size)) { fail("XBIN0111", "ELF symbol entry is truncated"); return result; }
            if (!read_c_string(bytes, static_cast<std::size_t>(strh.offset) + name_offset, str_end, symbol_names[i])) {
                fail("XBIN0112", "ELF symbol name is invalid"); return result;
            }
            const std::uint8_t info = bytes[at + 4U];
            const std::uint8_t binding = static_cast<std::uint8_t>(info >> 4U);
            const std::uint8_t symtype = static_cast<std::uint8_t>(info & 0x0fU);
            if (symtype == 3U || symbol_names[i].empty()) continue; // section symbols are represented by section records
            ObjectSymbol symbol;
            symbol.name = symbol_names[i]; symbol.binding = binding == 0U ? ObjectSymbolBinding::Local :
                (binding == 2U ? ObjectSymbolBinding::Weak : ObjectSymbolBinding::Global);
            symbol.kind = symtype == 1U ? ObjectSymbolKind::Object : ObjectSymbolKind::Function;
            symbol.defined = shndx != 0U;
            if (symbol.defined) {
                if (shndx >= section_index_to_model.size() || section_index_to_model[shndx] == InvalidObjectSectionId) {
                    fail("XBIN0113", "ELF symbol refers to unsupported section"); return result;
                }
                symbol.section_id = section_index_to_model[shndx]; symbol.value = static_cast<std::size_t>(value); symbol.size = static_cast<std::size_t>(size);
            }
            result.object_file.symbols.push_back(std::move(symbol));
        }
        for (std::size_t i = 1U; i < shnum; ++i) {
            const auto& relh = headers[i];
            if (relh.type != 4U) continue;
            if (relh.link != symtab_index || relh.info >= section_index_to_model.size() ||
                section_index_to_model[relh.info] == InvalidObjectSectionId || relh.entry_size != 24U ||
                relh.size % 24U != 0U || !range_ok(bytes.size(), relh.offset, relh.size)) {
                fail("XBIN0114", "ELF RELA section metadata is invalid"); return result;
            }
            for (std::size_t j = 0; j < relh.size / 24U; ++j) {
                const std::size_t at = static_cast<std::size_t>(relh.offset) + j * 24U;
                std::uint64_t offset = 0U, info = 0U, addend = 0U;
                get_u64(bytes, at, offset); get_u64(bytes, at + 8U, info); get_u64(bytes, at + 16U, addend);
                const auto symidx = static_cast<std::uint32_t>(info >> 32U);
                const auto kind = elf_kind(static_cast<std::uint32_t>(info & 0xffffffffU));
                if (symidx >= symbol_names.size() || symbol_names[symidx].empty() || kind == static_cast<ObjectRelocationKind>(255)) {
                    fail("XBIN0115", "ELF relocation references an unsupported symbol or relocation kind"); return result;
                }
                ObjectRelocation relocation;
                relocation.section_id = section_index_to_model[relh.info]; relocation.offset = static_cast<std::size_t>(offset);
                relocation.kind = kind; relocation.target_symbol = symbol_names[symidx];
                relocation.addend = static_cast<std::int64_t>(addend); relocation.width = kind == ObjectRelocationKind::Absolute64 ? 8U : 4U;
                result.object_file.relocations.push_back(std::move(relocation));
            }
        }
    }
    const auto validation = validate_object_file(result.object_file);
    if (!validation.succeeded()) result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
    return result;
}

BinaryObjectReadResult read_coff(const std::vector<std::uint8_t>& bytes) {
    BinaryObjectReadResult result;
    const auto fail = [&result](const char* code, const std::string& message) { diag(result.diagnostics, code, message); };
    std::uint16_t machine = 0U, section_count = 0U;
    std::uint32_t symbol_offset = 0U, symbol_count = 0U;
    if (bytes.size() < 20U || !get_u16(bytes, 0U, machine) || !get_u16(bytes, 2U, section_count) ||
        !get_u32(bytes, 8U, symbol_offset) || !get_u32(bytes, 12U, symbol_count)) { fail("XBIN0200", "input is not a complete COFF header"); return result; }
    if (machine != 0x8664U || section_count == 0U || !range_ok(bytes.size(), 20U, static_cast<std::uint64_t>(section_count) * 40U)) {
        fail("XBIN0201", "unsupported or malformed AMD64 COFF header"); return result;
    }
    const std::uint64_t symbytes = static_cast<std::uint64_t>(symbol_count) * 18U;
    if (symbol_count != 0U && (!range_ok(bytes.size(), symbol_offset, symbytes) || !range_ok(bytes.size(), static_cast<std::uint64_t>(symbol_offset) + symbytes, 4U))) {
        fail("XBIN0202", "COFF symbol table is out of bounds"); return result;
    }
    std::uint32_t string_size = 4U;
    if (symbol_offset != 0U && (!get_u32(bytes, static_cast<std::size_t>(symbol_offset + symbytes), string_size) ||
        string_size < 4U || !range_ok(bytes.size(), static_cast<std::uint64_t>(symbol_offset) + symbytes, string_size))) {
        fail("XBIN0203", "COFF string table is malformed"); return result;
    }
    const std::size_t string_start = static_cast<std::size_t>(symbol_offset + symbytes);
    const std::size_t string_end = string_start + string_size;
    result.object_file.object_id = "coff-amd64-read";
    result.object_file.descriptor = make_coff_x86_64_object_target();
    std::vector<ObjectSectionId> section_ids(section_count + 1U, InvalidObjectSectionId);
    std::vector<std::uint32_t> reloc_ptrs(section_count + 1U, 0U);
    std::vector<std::uint16_t> reloc_counts(section_count + 1U, 0U);
    for (std::size_t i = 0; i < section_count; ++i) {
        const std::size_t at = 20U + i * 40U;
        std::string name;
        if (bytes[at] == '/' ) {
            std::size_t n = at + 1U; while (n < at + 8U && bytes[n] >= '0' && bytes[n] <= '9') ++n;
            if (n == at + 1U) { fail("XBIN0204", "COFF long section name offset is invalid"); return result; }
            std::uint32_t offset = 0U;
            try { offset = static_cast<std::uint32_t>(std::stoul(std::string(reinterpret_cast<const char*>(bytes.data() + at + 1U), n - at - 1U))); }
            catch (...) { fail("XBIN0205", "COFF long section name offset is invalid"); return result; }
            if (!read_c_string(bytes, string_start + offset, string_end, name)) { fail("XBIN0206", "COFF long section name is out of bounds"); return result; }
        } else {
            std::size_t length = 0U; while (length < 8U && bytes[at + length] != 0U) ++length;
            name.assign(reinterpret_cast<const char*>(bytes.data() + at), length);
        }
        std::uint32_t virtual_size = 0U, raw_size = 0U, raw_ptr = 0U, reloc_ptr = 0U, characteristics = 0U;
        std::uint16_t reloc_count = 0U;
        get_u32(bytes, at + 8U, virtual_size); get_u32(bytes, at + 16U, raw_size); get_u32(bytes, at + 20U, raw_ptr);
        get_u32(bytes, at + 24U, reloc_ptr); get_u16(bytes, at + 32U, reloc_count); get_u32(bytes, at + 36U, characteristics);
        if (raw_size != 0U && !range_ok(bytes.size(), raw_ptr, raw_size)) { fail("XBIN0207", "COFF section raw data is out of bounds"); return result; }
        if (reloc_count != 0U && !range_ok(bytes.size(), reloc_ptr, static_cast<std::uint64_t>(reloc_count) * 10U)) { fail("XBIN0208", "COFF relocation table is out of bounds"); return result; }
        ObjectSection section;
        section.id = static_cast<ObjectSectionId>(i + 1U); section.name = name;
        section.comdat = (characteristics & 0x00001000U) != 0U;
        section.size = characteristics == 0U ? raw_size : ((characteristics & 0x00000080U) != 0U ? virtual_size : raw_size);
        const std::uint32_t alignment_code = (characteristics >> 20U) & 0x0fU;
        section.alignment = alignment_code >= 1U && alignment_code <= 14U
            ? (static_cast<std::size_t>(1U) << (alignment_code - 1U)) : 1U;
        if ((characteristics & 0x20000000U) != 0U) section.kind = ObjectSectionKind::Text;
        else if ((characteristics & 0x00000080U) != 0U) section.kind = ObjectSectionKind::Bss;
        else if ((characteristics & 0x80000000U) != 0U) section.kind = ObjectSectionKind::Data;
        else if (name.rfind(".pdata", 0U) == 0U || name.rfind(".xdata", 0U) == 0U)
            section.kind = ObjectSectionKind::Metadata;
        else section.kind = ObjectSectionKind::ReadOnlyData;
        if (section.kind != ObjectSectionKind::Bss && raw_size != 0U)
            section.contents.assign(bytes.begin() + static_cast<std::ptrdiff_t>(raw_ptr), bytes.begin() + static_cast<std::ptrdiff_t>(raw_ptr + raw_size));
        section_ids[i + 1U] = section.id; reloc_ptrs[i + 1U] = reloc_ptr; reloc_counts[i + 1U] = reloc_count;
        result.object_file.sections.push_back(std::move(section));
    }
    std::vector<std::string> symbol_names(symbol_count);
    for (std::size_t i = 0; i < symbol_count;) {
        const std::size_t at = static_cast<std::size_t>(symbol_offset) + i * 18U;
        std::string name;
        std::uint32_t zero = 0U, name_offset = 0U, value = 0U;
        get_u32(bytes, at, zero); get_u32(bytes, at + 4U, name_offset); get_u32(bytes, at + 8U, value);
        if (zero == 0U && name_offset != 0U) {
            if (!read_c_string(bytes, string_start + name_offset, string_end, name)) { fail("XBIN0209", "COFF symbol name is out of bounds"); return result; }
        } else {
            std::size_t length = 0U; while (length < 8U && bytes[at + length] != 0U) ++length;
            name.assign(reinterpret_cast<const char*>(bytes.data() + at), length);
        }
        std::uint16_t section_number = 0U, type = 0U; get_u16(bytes, at + 12U, section_number); get_u16(bytes, at + 14U, type);
        const std::uint8_t storage = bytes[at + 16U]; const std::uint8_t aux_count = bytes[at + 17U];
        if (aux_count > symbol_count - i - 1U) { fail("XBIN0210", "COFF auxiliary symbol count exceeds table"); return result; }
        ObjectSymbol symbol;
        symbol.name = name; symbol.binding = storage == 3U ? ObjectSymbolBinding::Local :
            (storage == 105U ? ObjectSymbolBinding::Weak : ObjectSymbolBinding::Global);
        symbol.kind = (type & 0x20U) != 0U ? ObjectSymbolKind::Function : ObjectSymbolKind::Object;
        symbol.defined = section_number > 0U;
        if (symbol.defined) {
            if (section_number >= section_ids.size() || section_ids[section_number] == InvalidObjectSectionId) { fail("XBIN0211", "COFF symbol section number is invalid"); return result; }
            symbol.section_id = section_ids[section_number]; symbol.value = value;
        }
        // Preserve IMAGE_COMDAT_SELECT_ASSOCIATIVE from section-definition
        // auxiliary records. Only a static section symbol whose name matches
        // the section header is interpreted as a section definition.
        if (aux_count != 0U && storage == 3U && type == 0U && section_number > 0U &&
            section_number < section_ids.size() &&
            name == result.object_file.sections[section_number - 1U].name) {
            const std::size_t aux = at + 18U;
            const std::uint8_t selection = bytes[aux + 14U];
            if (selection == 5U) { // IMAGE_COMDAT_SELECT_ASSOCIATIVE
                std::uint16_t associated_number = 0U;
                if (!get_u16(bytes, aux + 12U, associated_number) || associated_number == 0U ||
                    associated_number >= section_ids.size() ||
                    section_ids[associated_number] == InvalidObjectSectionId ||
                    associated_number == section_number) {
                    fail("XBIN0217", "COFF associative COMDAT auxiliary record has an invalid key section");
                    return result;
                }
                result.object_file.sections[section_number - 1U].associative_to_section_id =
                    section_ids[associated_number];
            }
        }
        symbol_names[i] = name;
        result.object_file.symbols.push_back(std::move(symbol));
        i += 1U + aux_count;
    }
    for (std::size_t section_number = 1U; section_number <= section_count; ++section_number) {
        for (std::size_t j = 0; j < reloc_counts[section_number]; ++j) {
            const std::size_t at = static_cast<std::size_t>(reloc_ptrs[section_number]) + j * 10U;
            std::uint32_t offset = 0U, symidx = 0U; std::uint16_t type = 0U;
            get_u32(bytes, at, offset); get_u32(bytes, at + 4U, symidx); get_u16(bytes, at + 8U, type);
            if (symidx >= symbol_names.size() || symbol_names[symidx].empty()) { fail("XBIN0212", "COFF relocation symbol index is invalid"); return result; }
            const auto kind = coff_kind(type);
            if (kind == static_cast<ObjectRelocationKind>(255)) { fail("XBIN0213", "COFF relocation type is unsupported"); return result; }
            ObjectRelocation relocation;
            relocation.section_id = section_ids[section_number]; relocation.offset = offset;
            relocation.kind = kind; relocation.target_symbol = symbol_names[symidx];
            relocation.width = kind == ObjectRelocationKind::CoffAddress64 ? 8U : 4U;
            const auto& source_section = result.object_file.sections[section_number - 1U];
            if (offset > source_section.contents.size() || relocation.width > source_section.contents.size() - std::min<std::size_t>(offset, source_section.contents.size())) {
                fail("XBIN0214", "COFF relocation addend field is outside raw section data"); return result;
            }
            if (relocation.width == 4U) {
                std::uint32_t raw_addend = 0U;
                if (!get_u32(source_section.contents, offset, raw_addend)) { fail("XBIN0215", "COFF relocation addend is truncated"); return result; }
                relocation.addend = kind == ObjectRelocationKind::CoffAddr32NB
                    ? static_cast<std::int64_t>(raw_addend)
                    : static_cast<std::int32_t>(raw_addend);
            } else {
                std::uint64_t raw_addend = 0U;
                if (!get_u64(source_section.contents, offset, raw_addend)) { fail("XBIN0216", "COFF relocation addend is truncated"); return result; }
                relocation.addend = static_cast<std::int64_t>(raw_addend);
            }
            result.object_file.relocations.push_back(std::move(relocation));
        }
    }
    const auto validation = validate_object_file(result.object_file);
    if (!validation.succeeded()) result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
    return result;
}

} // namespace

BinaryObjectWriteResult write_binary_object(const ObjectFile& object_file) {
    if (object_file.descriptor.format == ObjectFileFormat::ELF) return write_elf(object_file);
    if (object_file.descriptor.format == ObjectFileFormat::COFF) return write_coff(object_file);
    BinaryObjectWriteResult result;
    diag(result.diagnostics, "XBIN0030", "N23 binary writer currently supports ELF64 x86-64 and AMD64 COFF only");
    return result;
}

BinaryObjectReadResult read_binary_object(const std::vector<std::uint8_t>& bytes) {
    BinaryObjectReadResult result;
    if (bytes.size() >= 4U && bytes[0] == 0x7fU && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F')
        return read_elf(bytes);
    std::uint16_t machine = 0U;
    if (bytes.size() >= 2U && get_u16(bytes, 0U, machine) && machine == 0x8664U) return read_coff(bytes);
    diag(result.diagnostics, "XBIN0130", "input is neither a supported ELF64 nor AMD64 COFF object");
    return result;
}

} // namespace xfrontend
