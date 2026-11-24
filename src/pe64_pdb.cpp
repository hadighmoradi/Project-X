// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/pe64_pdb.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>

namespace xfrontend {
namespace {

constexpr std::uint32_t kCvSignatureC13 = 4U;
constexpr std::uint32_t kDebugS_SYMBOLS = 0xF1U;
constexpr std::uint16_t kS_OBJNAME = 0x1101U;
constexpr std::uint16_t kS_PUB32 = 0x110EU;
constexpr std::uint32_t kPublicFlagCode = 0x1U;
constexpr std::uint32_t kPublicFlagFunction = 0x2U;
constexpr std::size_t kMaximumDebugBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumPdbBytes = 1024U * 1024U * 1024U;
constexpr std::size_t kMaximumStreams = 65536U;
constexpr std::uint32_t kAbsentStream = 0xffffffffU;
constexpr char kMsfMagic[] = "Microsoft C/C++ MSF 7.00\r\n\032DS\0\0\0";

void add_error(std::vector<Diagnostic>& diagnostics, const char* code, const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, SourceSpan{}});
}

bool range_fits(std::size_t size, std::size_t offset, std::size_t length) {
    return offset <= size && length <= size - offset;
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i)
        bytes.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool printable_ascii(const std::string& value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x20U && ch <= 0x7eU;
    });
}

void append_codeview_record(std::vector<std::uint8_t>& stream, std::uint16_t kind,
                            const std::vector<std::uint8_t>& fields) {
    const std::size_t raw_size = 2U + fields.size(); // kind + fields, excluding length
    const std::size_t total_size = (2U + raw_size + 3U) & ~static_cast<std::size_t>(3U);
    const std::size_t record_length = total_size - 2U;
    append_u16(stream, static_cast<std::uint16_t>(record_length));
    append_u16(stream, kind);
    stream.insert(stream.end(), fields.begin(), fields.end());
    stream.resize(stream.size() + (total_size - 2U - raw_size), 0U);
}

void append_subsection(std::vector<std::uint8_t>& section, std::uint32_t kind,
                       const std::vector<std::uint8_t>& payload) {
    append_u32(section, kind);
    append_u32(section, static_cast<std::uint32_t>(payload.size()));
    section.insert(section.end(), payload.begin(), payload.end());
    while ((section.size() & 3U) != 0U) section.push_back(0U);
}

struct ParsedCoffDebugSection {
    std::string object_name;
    std::vector<Pe64CodeViewPublicSymbol> public_symbols;
    bool has_objname = false;
    bool has_symbols = false;
};

bool parse_debug_symbols(const std::vector<std::uint8_t>& data, ParsedCoffDebugSection& parsed,
                         std::string& error) {
    std::size_t position = 0U;
    std::set<std::string> names;
    while (position < data.size()) {
        if (!range_fits(data.size(), position, 4U)) {
            error = "CodeView symbol record header is truncated.";
            return false;
        }
        const std::uint16_t record_length = read_u16(data, position);
        if (record_length < 2U || !range_fits(data.size(), position + 2U, record_length)) {
            error = "CodeView symbol record length is invalid.";
            return false;
        }
        const std::uint16_t kind = read_u16(data, position + 2U);
        const std::size_t record_end = position + 2U + record_length;
        if (kind == kS_OBJNAME) {
            if (record_length < 8U || parsed.has_objname) {
                error = "S_OBJNAME record is truncated or duplicated.";
                return false;
            }
            const std::size_t name_start = position + 8U; // prefix + kind + signature
            const auto zero = std::find(data.begin() + static_cast<std::ptrdiff_t>(name_start),
                data.begin() + static_cast<std::ptrdiff_t>(record_end), 0U);
            if (zero == data.begin() + static_cast<std::ptrdiff_t>(record_end)) {
                error = "S_OBJNAME string is not terminated.";
                return false;
            }
            parsed.object_name.assign(data.begin() + static_cast<std::ptrdiff_t>(name_start), zero);
            if (!printable_ascii(parsed.object_name, 1024U)) {
                error = "S_OBJNAME string is not printable ASCII.";
                return false;
            }
            parsed.has_objname = true;
        } else if (kind == kS_PUB32) {
            if (record_length < 13U) {
                error = "S_PUB32 record is shorter than its fixed fields.";
                return false;
            }
            const std::uint32_t flags = read_u32(data, position + 4U);
            const std::uint32_t offset = read_u32(data, position + 8U);
            const std::uint16_t section = read_u16(data, position + 12U);
            const std::size_t name_start = position + 14U;
            const auto zero = std::find(data.begin() + static_cast<std::ptrdiff_t>(name_start),
                data.begin() + static_cast<std::ptrdiff_t>(record_end), 0U);
            if (zero == data.begin() + static_cast<std::ptrdiff_t>(record_end) || section == 0U) {
                error = "S_PUB32 has an unterminated name or zero section index.";
                return false;
            }
            std::string name(data.begin() + static_cast<std::ptrdiff_t>(name_start), zero);
            if (!printable_ascii(name, 4096U) || !names.insert(name).second) {
                error = "S_PUB32 name is invalid or duplicated.";
                return false;
            }
            Pe64CodeViewPublicSymbol symbol;
            symbol.name = std::move(name);
            symbol.section_id = section;
            symbol.offset = offset;
            symbol.kind = ((flags & (kPublicFlagCode | kPublicFlagFunction)) != 0U)
                ? Pe64CodeViewPublicKind::Function : Pe64CodeViewPublicKind::Data;
            parsed.public_symbols.push_back(std::move(symbol));
        } else {
            error = "Unsupported CodeView symbol record kind in N45 subset.";
            return false;
        }
        position = record_end;
    }
    return true;
}

bool read_msf_stream(const std::vector<std::uint8_t>& bytes, std::uint32_t page_size,
                     const std::vector<std::uint32_t>& stream_sizes,
                     const std::vector<std::vector<std::uint32_t>>& stream_pages,
                     std::size_t stream_index, std::vector<std::uint8_t>& output,
                     std::string& error) {
    if (stream_index >= stream_sizes.size() || stream_sizes[stream_index] == kAbsentStream) {
        error = "Required PDB stream is absent.";
        return false;
    }
    const std::size_t stream_size = stream_sizes[stream_index];
    const std::size_t expected_pages = (stream_size + page_size - 1U) / page_size;
    if (stream_pages[stream_index].size() != expected_pages) {
        error = "PDB stream page count does not match its declared size.";
        return false;
    }
    output.clear();
    output.reserve(stream_size);
    for (const std::uint32_t page : stream_pages[stream_index]) {
        if (page >= bytes.size() / page_size) {
            error = "PDB stream references a page outside the file.";
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(page) * page_size;
        const std::size_t amount = std::min<std::size_t>(page_size, stream_size - output.size());
        if (!range_fits(bytes.size(), offset, amount)) {
            error = "PDB stream page is truncated.";
            return false;
        }
        output.insert(output.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + amount));
    }
    return output.size() == stream_size;
}

} // namespace

Pe64CodeViewCoffResult write_pe64_codeview_coff_object(
    const ObjectFile& object, const Pe64CodeViewCoffSpec& spec) {
    Pe64CodeViewCoffResult result;
    if (object.descriptor.format != ObjectFileFormat::COFF ||
        object.descriptor.architecture != ObjectArchitecture::X86_64 ||
        object.descriptor.operating_system != ObjectOperatingSystem::Windows) {
        add_error(result.diagnostics, "XPE64PDB0001", "N45 CodeView writer requires an AMD64 Windows COFF object.");
        return result;
    }
    const auto validation = validate_object_file(object);
    if (!validation.succeeded()) {
        result.diagnostics = validation.diagnostics;
        return result;
    }
    if (!printable_ascii(spec.object_name, 1024U) || spec.public_symbols.empty() ||
        spec.public_symbols.size() > 65535U) {
        add_error(result.diagnostics, "XPE64PDB0002", "Object name must be 1..1024 printable ASCII bytes and one or more public symbols are required.");
        return result;
    }
    if (std::any_of(object.sections.begin(), object.sections.end(), [](const ObjectSection& section) {
        return section.name == ".debug$S" || section.name == ".debug$T";
    })) {
        add_error(result.diagnostics, "XPE64PDB0003", "Input object already contains CodeView debug sections.");
        return result;
    }

    std::map<ObjectSectionId, std::size_t> section_indices;
    for (std::size_t i = 0U; i < object.sections.size(); ++i)
        section_indices.emplace(object.sections[i].id, i + 1U);

    std::set<std::string> emitted_names;
    std::vector<std::uint8_t> symbols;
    std::vector<std::uint8_t> objname_fields;
    append_u32(objname_fields, 0U);
    objname_fields.insert(objname_fields.end(), spec.object_name.begin(), spec.object_name.end());
    objname_fields.push_back(0U);
    append_codeview_record(symbols, kS_OBJNAME, objname_fields);

    for (const auto& public_symbol : spec.public_symbols) {
        if (!printable_ascii(public_symbol.name, 4096U) || !emitted_names.insert(public_symbol.name).second) {
            add_error(result.diagnostics, "XPE64PDB0004", "Public symbol name is invalid or duplicated.");
            return result;
        }
        const auto symbol_it = std::find_if(object.symbols.begin(), object.symbols.end(), [&public_symbol](const ObjectSymbol& item) {
            return item.name == public_symbol.name;
        });
        const auto section_it = std::find_if(object.sections.begin(), object.sections.end(), [&public_symbol](const ObjectSection& item) {
            return item.id == public_symbol.section_id;
        });
        if (symbol_it == object.symbols.end() || !symbol_it->defined ||
            (symbol_it->binding != ObjectSymbolBinding::Global && symbol_it->binding != ObjectSymbolBinding::Weak) ||
            symbol_it->section_id != public_symbol.section_id || symbol_it->value != public_symbol.offset ||
            section_it == object.sections.end() || section_it->kind == ObjectSectionKind::Bss ||
            section_it->kind == ObjectSectionKind::Metadata || public_symbol.offset >= section_it->size ||
            section_indices[public_symbol.section_id] > 0xffffU) {
            add_error(result.diagnostics, "XPE64PDB0005", "Public CodeView symbol must match a defined external COFF symbol in a file-backed section.");
            return result;
        }
        if ((public_symbol.kind == Pe64CodeViewPublicKind::Function &&
             (symbol_it->kind != ObjectSymbolKind::Function || section_it->kind != ObjectSectionKind::Text)) ||
            (public_symbol.kind == Pe64CodeViewPublicKind::Data &&
             (symbol_it->kind != ObjectSymbolKind::Object || section_it->kind == ObjectSectionKind::Text))) {
            add_error(result.diagnostics, "XPE64PDB0006", "CodeView public symbol kind does not match its COFF symbol/section kind.");
            return result;
        }
        std::vector<std::uint8_t> fields;
        const std::uint32_t flags = public_symbol.kind == Pe64CodeViewPublicKind::Function
            ? (kPublicFlagCode | kPublicFlagFunction) : 0U;
        append_u32(fields, flags);
        append_u32(fields, public_symbol.offset);
        append_u16(fields, static_cast<std::uint16_t>(section_indices[public_symbol.section_id]));
        fields.insert(fields.end(), public_symbol.name.begin(), public_symbol.name.end());
        fields.push_back(0U);
        append_codeview_record(symbols, kS_PUB32, fields);
    }

    std::vector<std::uint8_t> debug_section;
    append_u32(debug_section, kCvSignatureC13);
    append_subsection(debug_section, kDebugS_SYMBOLS, symbols);
    if (debug_section.size() > kMaximumDebugBytes) {
        add_error(result.diagnostics, "XPE64PDB0007", "Generated CodeView debug section exceeds the 16 MiB limit.");
        return result;
    }

    ObjectFile encoded_object = object;
    ObjectSection debug;
    ObjectSectionId next_id = 1U;
    for (const auto& section : encoded_object.sections) {
        if (section.id >= next_id) {
            if (section.id == std::numeric_limits<ObjectSectionId>::max()) {
                add_error(result.diagnostics, "XPE64PDB0008", "No ObjectSectionId remains for .debug$S.");
                return result;
            }
            next_id = section.id + 1U;
        }
    }
    debug.id = next_id;
    debug.name = ".debug$S";
    debug.kind = ObjectSectionKind::Metadata;
    debug.alignment = 4U;
    debug.size = debug_section.size();
    debug.contents = std::move(debug_section);
    encoded_object.sections.push_back(std::move(debug));

    const auto encoded = write_binary_object(encoded_object);
    if (!encoded.succeeded()) {
        result.diagnostics = encoded.diagnostics;
        return result;
    }
    result.bytes = encoded.bytes;
    result.public_symbol_count = spec.public_symbols.size();
    result.debug_section_bytes = encoded_object.sections.back().contents.size();
    return result;
}

Pe64CodeViewCoffInspection inspect_pe64_codeview_coff_object(
    const std::vector<std::uint8_t>& object_bytes) {
    Pe64CodeViewCoffInspection result;
    const auto decoded = read_binary_object(object_bytes);
    if (!decoded.succeeded()) {
        result.error = decoded.diagnostics.empty() ? "COFF object decoding failed." : decoded.diagnostics.front().message;
        return result;
    }
    if (decoded.object_file.descriptor.format != ObjectFileFormat::COFF ||
        decoded.object_file.descriptor.architecture != ObjectArchitecture::X86_64) {
        result.error = "CodeView inspector requires an AMD64 COFF object.";
        return result;
    }
    const auto found = std::find_if(decoded.object_file.sections.begin(), decoded.object_file.sections.end(), [](const ObjectSection& section) {
        return section.name == ".debug$S";
    });
    if (found == decoded.object_file.sections.end()) {
        result.error = "COFF object has no .debug$S section.";
        return result;
    }
    result.present = true;
    const auto& data = found->contents;
    if (data.size() < 12U || data.size() > kMaximumDebugBytes || read_u32(data, 0U) != kCvSignatureC13) {
        result.error = "CodeView C13 signature or section size is invalid.";
        return result;
    }
    std::size_t position = 4U;
    ParsedCoffDebugSection parsed;
    while (position < data.size()) {
        if (!range_fits(data.size(), position, 8U)) {
            result.error = "CodeView subsection header is truncated.";
            return result;
        }
        const std::uint32_t kind = read_u32(data, position);
        const std::uint32_t length = read_u32(data, position + 4U);
        position += 8U;
        if (!range_fits(data.size(), position, length)) {
            result.error = "CodeView subsection extends beyond .debug$S.";
            return result;
        }
        if (kind == kDebugS_SYMBOLS) {
            if (parsed.has_symbols) {
                result.error = "Duplicate DEBUG_S_SYMBOLS subsection.";
                return result;
            }
            std::vector<std::uint8_t> payload(data.begin() + static_cast<std::ptrdiff_t>(position),
                data.begin() + static_cast<std::ptrdiff_t>(position + length));
            if (!parse_debug_symbols(payload, parsed, result.error)) return result;
            parsed.has_symbols = true;
        }
        position += length;
        const std::size_t aligned = (position + 3U) & ~static_cast<std::size_t>(3U);
        if (aligned > data.size()) {
            result.error = "CodeView subsection alignment exceeds .debug$S.";
            return result;
        }
        for (std::size_t i = position; i < aligned; ++i) {
            if (data[i] != 0U) {
                result.error = "CodeView subsection padding must be zero.";
                return result;
            }
        }
        position = aligned;
    }
    if (!parsed.has_objname || !parsed.has_symbols || parsed.public_symbols.empty()) {
        result.error = "CodeView C13 data must contain S_OBJNAME and at least one S_PUB32.";
        return result;
    }
    for (const auto& symbol : parsed.public_symbols) {
        if (symbol.section_id == 0U || symbol.section_id > decoded.object_file.sections.size()) {
            result.error = "S_PUB32 refers to a COFF section outside the section table.";
            return result;
        }
        const auto& target = decoded.object_file.sections[symbol.section_id - 1U];
        if (symbol.offset >= target.size || target.kind == ObjectSectionKind::Bss ||
            target.kind == ObjectSectionKind::Metadata) {
            result.error = "S_PUB32 section/offset does not identify file-backed program data.";
            return result;
        }
    }
    result.object_name = std::move(parsed.object_name);
    result.public_symbols = std::move(parsed.public_symbols);
    result.valid = true;
    return result;
}

Pe64PdbInspection inspect_pe64_msf_pdb(const std::vector<std::uint8_t>& pdb_bytes) {
    Pe64PdbInspection result;
    result.present = !pdb_bytes.empty();
    if (pdb_bytes.size() < 56U || pdb_bytes.size() > kMaximumPdbBytes ||
        !std::equal(std::begin(kMsfMagic), std::end(kMsfMagic) - 1, pdb_bytes.begin())) {
        result.error = "PDB does not have a valid MSF 7.0 superblock signature or file size.";
        return result;
    }
    const std::uint32_t page_size = read_u32(pdb_bytes, 32U);
    const std::uint32_t page_count = read_u32(pdb_bytes, 40U);
    const std::uint32_t directory_bytes = read_u32(pdb_bytes, 44U);
    const std::uint32_t block_map_page = read_u32(pdb_bytes, 52U);
    if (page_size < 512U || page_size > 4096U || (page_size & (page_size - 1U)) != 0U ||
        page_count == 0U || static_cast<std::uint64_t>(page_count) * page_size != pdb_bytes.size() ||
        directory_bytes == 0U || directory_bytes > 64U * 1024U * 1024U || block_map_page >= page_count) {
        result.error = "MSF 7.0 page size, page count, directory size or block-map page is invalid.";
        return result;
    }
    const std::size_t directory_pages = (static_cast<std::size_t>(directory_bytes) + page_size - 1U) / page_size;
    const std::size_t block_map_offset = static_cast<std::size_t>(block_map_page) * page_size;
    if (!range_fits(pdb_bytes.size(), block_map_offset, directory_pages * 4U)) {
        result.error = "MSF directory block map is truncated.";
        return result;
    }
    std::vector<std::uint8_t> directory;
    directory.reserve(directory_bytes);
    for (std::size_t i = 0U; i < directory_pages; ++i) {
        const std::uint32_t page = read_u32(pdb_bytes, block_map_offset + i * 4U);
        if (page >= page_count) {
            result.error = "MSF directory references a page outside the file.";
            return result;
        }
        const std::size_t offset = static_cast<std::size_t>(page) * page_size;
        const std::size_t amount = std::min<std::size_t>(page_size, directory_bytes - directory.size());
        directory.insert(directory.end(), pdb_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            pdb_bytes.begin() + static_cast<std::ptrdiff_t>(offset + amount));
    }
    if (directory.size() != directory_bytes || directory.size() < 4U) {
        result.error = "MSF stream directory is truncated.";
        return result;
    }
    const std::uint32_t stream_count = read_u32(directory, 0U);
    if (stream_count < 2U || stream_count > kMaximumStreams ||
        !range_fits(directory.size(), 4U, static_cast<std::size_t>(stream_count) * 4U)) {
        result.error = "MSF stream count or stream-size array is invalid.";
        return result;
    }
    std::vector<std::uint32_t> stream_sizes(stream_count);
    std::size_t position = 4U;
    for (std::size_t i = 0U; i < stream_count; ++i) {
        stream_sizes[i] = read_u32(directory, position);
        position += 4U;
        if (stream_sizes[i] != kAbsentStream && stream_sizes[i] > kMaximumPdbBytes) {
            result.error = "MSF stream exceeds the PDB file-size limit.";
            return result;
        }
    }
    std::vector<std::vector<std::uint32_t>> stream_pages(stream_count);
    for (std::size_t i = 0U; i < stream_count; ++i) {
        if (stream_sizes[i] == kAbsentStream) continue;
        const std::size_t count = (static_cast<std::size_t>(stream_sizes[i]) + page_size - 1U) / page_size;
        if (!range_fits(directory.size(), position, count * 4U)) {
            result.error = "MSF stream page list is truncated.";
            return result;
        }
        stream_pages[i].reserve(count);
        for (std::size_t j = 0U; j < count; ++j) {
            const std::uint32_t page = read_u32(directory, position);
            position += 4U;
            if (page >= page_count) {
                result.error = "MSF stream references a page outside the file.";
                return result;
            }
            stream_pages[i].push_back(page);
        }
    }
    std::vector<std::uint8_t> info;
    std::string error;
    if (!read_msf_stream(pdb_bytes, page_size, stream_sizes, stream_pages, 1U, info, error)) {
        result.error = error;
        return result;
    }
    if (info.size() < 28U) {
        result.error = "PDB Information stream is shorter than its fixed identity header.";
        return result;
    }
    result.version = read_u32(info, 0U);
    result.signature = read_u32(info, 4U);
    result.age = read_u32(info, 8U);
    for (std::size_t i = 0U; i < result.guid.size(); ++i) result.guid[i] = info[12U + i];
    if (result.age == 0U || std::all_of(result.guid.begin(), result.guid.end(), [](std::uint8_t value) { return value == 0U; })) {
        result.error = "PDB Information stream contains an empty GUID or zero age.";
        return result;
    }
    result.page_size = page_size;
    result.page_count = page_count;
    result.stream_count = stream_count;
    result.valid = true;
    return result;
}

Pe64PdbPairInspection verify_pe64_pdb_pair(
    const std::vector<std::uint8_t>& pe_bytes,
    const std::vector<std::uint8_t>& pdb_bytes) {
    Pe64PdbPairInspection result;
    result.codeview = inspect_pe64_codeview_debug_directory(pe_bytes);
    result.pdb = inspect_pe64_msf_pdb(pdb_bytes);
    if (!result.codeview.valid || !result.codeview.present) {
        result.error = "PE CodeView RSDS identity is invalid: " + result.codeview.error;
        return result;
    }
    if (!result.pdb.valid) {
        result.error = "PDB MSF identity is invalid: " + result.pdb.error;
        return result;
    }
    result.guid_matches = result.codeview.guid == result.pdb.guid;
    result.age_matches = result.codeview.age == result.pdb.age;
    result.valid = result.guid_matches && result.age_matches;
    if (!result.valid) result.error = "PE RSDS GUID/Age does not match the PDB Information stream.";
    return result;
}

} // namespace xfrontend
