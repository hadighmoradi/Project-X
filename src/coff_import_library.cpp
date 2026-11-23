#include "xfrontend/coff_import_library.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xfrontend {
namespace {
constexpr std::size_t ImportHeaderSize = 20U;
constexpr std::uint16_t ImportSig2 = 0xffffU;
constexpr std::uint8_t CoffStorageClassExternal = 2U;
constexpr std::uint8_t CoffStorageClassStatic = 3U;
constexpr std::uint8_t CoffStorageClassSection = 104U;

void add_diag(std::vector<Diagnostic>& diagnostics, const char* code,
              const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}

void append_u16_le(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32_le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
}

bool read_u16_le(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                 std::uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
    return true;
}

bool read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                 std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    return true;
}

bool valid_ascii_name(const std::string& value) {
    if (value.empty()) return false;
    for (const unsigned char ch : value) {
        if (ch < 0x20U || ch > 0x7eU) return false;
    }
    return true;
}

void patch_u32_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

void append_zeroes(std::vector<std::uint8_t>& bytes, std::size_t count) {
    bytes.insert(bytes.end(), count, 0U);
}

void append_coff_file_header(std::vector<std::uint8_t>& bytes, std::uint16_t section_count,
                             std::uint32_t symbol_table_offset, std::uint32_t symbol_count) {
    append_u16_le(bytes, CoffMachineAmd64);
    append_u16_le(bytes, section_count);
    append_u32_le(bytes, 0U); // TimeDateStamp.
    append_u32_le(bytes, symbol_table_offset);
    append_u32_le(bytes, symbol_count);
    append_u16_le(bytes, 0U); // SizeOfOptionalHeader.
    append_u16_le(bytes, 0U); // Characteristics: 64-bit object.
}

void append_coff_section_header(std::vector<std::uint8_t>& bytes, const char* name,
                                std::uint32_t raw_size, std::uint32_t raw_pointer,
                                std::uint32_t relocation_pointer, std::uint16_t relocation_count,
                                std::uint32_t characteristics) {
    const std::size_t name_length = std::char_traits<char>::length(name);
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes.push_back(index < name_length ? static_cast<std::uint8_t>(name[index]) : 0U);
    }
    append_u32_le(bytes, 0U); // VirtualSize.
    append_u32_le(bytes, 0U); // VirtualAddress.
    append_u32_le(bytes, raw_size);
    append_u32_le(bytes, raw_pointer);
    append_u32_le(bytes, relocation_pointer);
    append_u32_le(bytes, 0U); // PointerToLinenumbers.
    append_u16_le(bytes, relocation_count);
    append_u16_le(bytes, 0U); // NumberOfLinenumbers.
    append_u32_le(bytes, characteristics);
}

void append_coff_relocation(std::vector<std::uint8_t>& bytes, std::uint32_t offset,
                            std::uint32_t symbol_index, std::uint16_t type) {
    append_u32_le(bytes, offset);
    append_u32_le(bytes, symbol_index);
    append_u16_le(bytes, type);
}

void append_coff_symbol(std::vector<std::uint8_t>& symbols,
                        std::vector<std::uint8_t>& string_table,
                        const std::string& name, std::uint32_t value,
                        std::int16_t section_number, std::uint16_t type,
                        std::uint8_t storage_class, std::uint8_t auxiliary_count = 0U) {
    if (name.size() <= 8U) {
        for (std::size_t index = 0U; index < 8U; ++index) {
            symbols.push_back(index < name.size() ? static_cast<std::uint8_t>(name[index]) : 0U);
        }
    } else {
        append_u32_le(symbols, 0U);
        const auto name_offset = static_cast<std::uint32_t>(string_table.size());
        append_u32_le(symbols, name_offset);
        string_table.insert(string_table.end(), name.begin(), name.end());
        string_table.push_back(0U);
    }
    append_u32_le(symbols, value);
    append_u16_le(symbols, static_cast<std::uint16_t>(section_number));
    append_u16_le(symbols, type);
    symbols.push_back(storage_class);
    symbols.push_back(auxiliary_count);
}

void append_coff_symbol_table(std::vector<std::uint8_t>& bytes,
                              const std::vector<std::uint8_t>& symbols,
                              std::vector<std::uint8_t> string_table) {
    patch_u32_le(string_table, 0U, static_cast<std::uint32_t>(string_table.size()));
    bytes.insert(bytes.end(), symbols.begin(), symbols.end());
    bytes.insert(bytes.end(), string_table.begin(), string_table.end());
}

std::string import_library_stem(const std::string& dll_name) {
    const auto separator = dll_name.find_last_of("/\\");
    const std::string basename = separator == std::string::npos
        ? dll_name : dll_name.substr(separator + 1U);
    const auto extension = basename.find_last_of('.');
    if (extension == std::string::npos || extension == 0U) return basename;
    return basename.substr(0U, extension);
}

std::string import_library_member_name(const std::string& dll_name) {
    const auto separator = dll_name.find_last_of("/\\");
    return separator == std::string::npos ? dll_name : dll_name.substr(separator + 1U);
}

ArchiveContainerMember make_import_descriptor_member(const std::string& dll_name,
                                                       const std::string& library_stem,
                                                       const std::string& archive_member_name) {
    constexpr std::uint32_t SectionHeadersEnd = 20U + 2U * 40U;
    constexpr std::uint32_t DescriptorSize = 20U;
    constexpr std::uint32_t RelocationSize = 10U;
    constexpr std::uint16_t RelAddr32Nb = 0x0003U;
    constexpr std::uint32_t Id2Characteristics = 0xc0300040U;
    constexpr std::uint32_t Id6Characteristics = 0xc0200040U;
    const std::string descriptor_symbol = "__IMPORT_DESCRIPTOR_" + library_stem;
    const std::string null_thunk_symbol = std::string(1U, static_cast<char>(0x7fU)) +
                                          library_stem + "_NULL_THUNK_DATA";
    const auto id2_raw = SectionHeadersEnd;
    const auto id2_reloc = id2_raw + DescriptorSize;
    const auto id6_raw = id2_reloc + 3U * RelocationSize;
    const auto symbol_table = id6_raw + static_cast<std::uint32_t>(dll_name.size() + 1U);

    std::vector<std::uint8_t> bytes;
    bytes.reserve(symbol_table + 7U * 18U + 128U);
    append_coff_file_header(bytes, 2U, symbol_table, 7U);
    append_coff_section_header(bytes, ".idata$2", DescriptorSize, id2_raw,
                               id2_reloc, 3U, Id2Characteristics);
    append_coff_section_header(bytes, ".idata$6", static_cast<std::uint32_t>(dll_name.size() + 1U),
                               id6_raw, 0U, 0U, Id6Characteristics);
    append_zeroes(bytes, DescriptorSize); // IMAGE_IMPORT_DESCRIPTOR; linker fills its RVAs.
    append_coff_relocation(bytes, 12U, 2U, RelAddr32Nb); // NameRVA -> .idata$6.
    append_coff_relocation(bytes, 0U, 3U, RelAddr32Nb);  // OriginalFirstThunk -> .idata$4.
    append_coff_relocation(bytes, 16U, 4U, RelAddr32Nb); // FirstThunk -> .idata$5.
    bytes.insert(bytes.end(), dll_name.begin(), dll_name.end());
    bytes.push_back(0U);

    std::vector<std::uint8_t> symbols;
    std::vector<std::uint8_t> strings(4U, 0U);
    append_coff_symbol(symbols, strings, descriptor_symbol, 0U, 1, 0U, CoffStorageClassExternal);
    append_coff_symbol(symbols, strings, ".idata$2", 0U, 1, 0U, CoffStorageClassSection);
    append_coff_symbol(symbols, strings, ".idata$6", 0U, 2, 0U, CoffStorageClassStatic);
    append_coff_symbol(symbols, strings, ".idata$4", 0U, 0, 0U, CoffStorageClassSection);
    append_coff_symbol(symbols, strings, ".idata$5", 0U, 0, 0U, CoffStorageClassSection);
    append_coff_symbol(symbols, strings, "__NULL_IMPORT_DESCRIPTOR", 0U, 0, 0U, CoffStorageClassExternal);
    append_coff_symbol(symbols, strings, null_thunk_symbol, 0U, 0, 0U, CoffStorageClassExternal);
    append_coff_symbol_table(bytes, symbols, std::move(strings));

    ArchiveContainerMember member;
    member.name = archive_member_name;
    member.contents = std::move(bytes);
    member.defined_symbols.push_back(descriptor_symbol);
    return member;
}

ArchiveContainerMember make_null_import_descriptor_member(const std::string& archive_member_name) {
    constexpr std::uint32_t SectionHeadersEnd = 20U + 40U;
    constexpr std::uint32_t DescriptorSize = 20U;
    constexpr std::uint32_t Characteristics = 0xc0300040U;
    const std::uint32_t symbol_table = SectionHeadersEnd + DescriptorSize;
    std::vector<std::uint8_t> bytes;
    append_coff_file_header(bytes, 1U, symbol_table, 1U);
    append_coff_section_header(bytes, ".idata$3", DescriptorSize, SectionHeadersEnd,
                               0U, 0U, Characteristics);
    append_zeroes(bytes, DescriptorSize);
    std::vector<std::uint8_t> symbols;
    std::vector<std::uint8_t> strings(4U, 0U);
    append_coff_symbol(symbols, strings, "__NULL_IMPORT_DESCRIPTOR", 0U, 1, 0U, 2U);
    append_coff_symbol_table(bytes, symbols, std::move(strings));

    ArchiveContainerMember member;
    member.name = archive_member_name;
    member.contents = std::move(bytes);
    member.defined_symbols.push_back("__NULL_IMPORT_DESCRIPTOR");
    return member;
}

ArchiveContainerMember make_null_thunk_member(const std::string& library_stem,
                                               const std::string& archive_member_name) {
    constexpr std::uint32_t SectionHeadersEnd = 20U + 2U * 40U;
    constexpr std::uint32_t ThunkSize = 8U;
    constexpr std::uint32_t Characteristics = 0xc0400040U;
    const std::string symbol_name = std::string(1U, static_cast<char>(0x7fU)) +
                                    library_stem + "_NULL_THUNK_DATA";
    const std::uint32_t symbol_table = SectionHeadersEnd + 2U * ThunkSize;
    std::vector<std::uint8_t> bytes;
    append_coff_file_header(bytes, 2U, symbol_table, 1U);
    append_coff_section_header(bytes, ".idata$5", ThunkSize, SectionHeadersEnd,
                               0U, 0U, Characteristics);
    append_coff_section_header(bytes, ".idata$4", ThunkSize, SectionHeadersEnd + ThunkSize,
                               0U, 0U, Characteristics);
    append_zeroes(bytes, 2U * ThunkSize);
    std::vector<std::uint8_t> symbols;
    std::vector<std::uint8_t> strings(4U, 0U);
    append_coff_symbol(symbols, strings, symbol_name, 0U, 1, 0U, 2U);
    append_coff_symbol_table(bytes, symbols, std::move(strings));

    ArchiveContainerMember member;
    member.name = archive_member_name;
    member.contents = std::move(bytes);
    member.defined_symbols.push_back(symbol_name);
    return member;
}

bool looks_like_short_import(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 4U && bytes[0U] == 0U && bytes[1U] == 0U &&
           bytes[2U] == 0xffU && bytes[3U] == 0xffU;
}

bool valid_type(CoffImportType type) {
    return type == CoffImportType::Code || type == CoffImportType::Data ||
           type == CoffImportType::Const;
}

bool valid_name_type(CoffImportNameType type) {
    return type == CoffImportNameType::Ordinal || type == CoffImportNameType::Name ||
           type == CoffImportNameType::NameNoPrefix ||
           type == CoffImportNameType::NameUndecorate;
}

bool validate_import(const CoffImportSymbol& import_symbol,
                     std::vector<Diagnostic>& diagnostics) {
    if (!valid_ascii_name(import_symbol.dll_name) ||
        !valid_ascii_name(import_symbol.symbol_name)) {
        add_diag(diagnostics, "XIMP0001",
                 "DLL and import symbol names must be non-empty printable ASCII without NUL bytes");
        return false;
    }
    if (!valid_type(import_symbol.type)) {
        add_diag(diagnostics, "XIMP0002", "unsupported COFF import type");
        return false;
    }
    if (!valid_name_type(import_symbol.name_type)) {
        add_diag(diagnostics, "XIMP0003", "unsupported COFF import name type");
        return false;
    }
    if (import_symbol.name_type == CoffImportNameType::Ordinal &&
        import_symbol.ordinal_or_hint == 0U) {
        add_diag(diagnostics, "XIMP0004", "ordinal imports require a non-zero ordinal");
        return false;
    }
    const std::size_t max_payload = std::numeric_limits<std::uint32_t>::max();
    if (import_symbol.symbol_name.size() > max_payload - 2U ||
        import_symbol.dll_name.size() > max_payload - 2U - import_symbol.symbol_name.size()) {
        add_diag(diagnostics, "XIMP0026", "short-import string payload exceeds the 32-bit SizeOfData field");
        return false;
    }
    if (!import_symbol.member_name.empty() &&
        (import_symbol.member_name.find('\0') != std::string::npos ||
         import_symbol.member_name.find('\n') != std::string::npos ||
         import_symbol.member_name.find('\r') != std::string::npos)) {
        add_diag(diagnostics, "XIMP0005", "import member name contains a forbidden NUL or newline");
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_short_import(const CoffImportSymbol& import_symbol,
                                              std::uint16_t machine) {
    const std::size_t payload_size = import_symbol.symbol_name.size() + 1U +
                                     import_symbol.dll_name.size() + 1U;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ImportHeaderSize + payload_size);
    append_u16_le(bytes, 0U); // Sig1: IMAGE_FILE_MACHINE_UNKNOWN.
    append_u16_le(bytes, ImportSig2);
    append_u16_le(bytes, 0U); // Version.
    append_u16_le(bytes, machine);
    append_u32_le(bytes, import_symbol.timestamp);
    append_u32_le(bytes, static_cast<std::uint32_t>(payload_size));
    append_u16_le(bytes, import_symbol.ordinal_or_hint);
    const auto type_info = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(import_symbol.type) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(import_symbol.name_type) << 2U));
    append_u16_le(bytes, type_info);
    bytes.insert(bytes.end(), import_symbol.symbol_name.begin(), import_symbol.symbol_name.end());
    bytes.push_back(0U);
    bytes.insert(bytes.end(), import_symbol.dll_name.begin(), import_symbol.dll_name.end());
    bytes.push_back(0U);
    return bytes;
}

bool append_import_symbols(const CoffShortImportObject& import_object,
                           std::set<std::string>& all_symbols,
                           std::vector<std::string>& member_symbols,
                           std::vector<Diagnostic>& diagnostics) {
    member_symbols = coff_import_linker_symbols(import_object);
    for (const auto& symbol : member_symbols) {
        if (!all_symbols.insert(symbol).second) {
            add_diag(diagnostics, "XIMP0006",
                     "two import-library members provide the same linker symbol: " + symbol);
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::string> coff_import_linker_symbols(
    const CoffShortImportObject& import_object) {
    std::vector<std::string> symbols;
    symbols.reserve(2U);
    symbols.push_back("__imp_" + import_object.symbol_name);
    if (import_object.type == CoffImportType::Code ||
        import_object.type == CoffImportType::Const) {
        symbols.push_back(import_object.symbol_name);
    }
    return symbols;
}

CoffImportLibraryWriteResult write_coff_import_library(
    const std::vector<CoffImportSymbol>& imports, std::uint16_t machine) {
    CoffImportLibraryWriteResult result;
    if (machine != CoffMachineAmd64) {
        add_diag(result.diagnostics, "XIMP0007", "N32 short-import writer currently supports AMD64 only");
        return result;
    }
    if (imports.empty()) {
        add_diag(result.diagnostics, "XIMP0008", "COFF import library requires at least one import");
        return result;
    }
    if (imports.size() > std::numeric_limits<std::uint16_t>::max()) {
        add_diag(result.diagnostics, "XIMP0009", "COFF import library exceeds the 65535-member limit");
        return result;
    }

    // A COFF import library groups every object for a DLL under the same
    // archive-member name (normally the DLL basename, e.g. KERNEL32.dll).
    // Section-class relocations in the descriptor resolve .idata$4/.idata$5
    // against matching member names in the same archive.
    std::map<std::string, std::pair<std::string, std::string>> libraries;
    std::map<std::string, std::string> member_name_to_library_stem;
    std::set<std::string> all_symbols;
    std::vector<std::string> validated_names;
    validated_names.reserve(imports.size());
    for (std::size_t index = 0U; index < imports.size(); ++index) {
        const auto& import_symbol = imports[index];
        if (!validate_import(import_symbol, result.diagnostics)) return result;
        const std::string library_stem = import_library_stem(import_symbol.dll_name);
        if (library_stem.empty() || !valid_ascii_name(library_stem)) {
            add_diag(result.diagnostics, "XIMP0027", "DLL name must produce a non-empty printable import-library stem");
            return result;
        }
        const std::string member_name = import_symbol.member_name.empty()
            ? import_library_member_name(import_symbol.dll_name) : import_symbol.member_name;
        if (!valid_ascii_name(member_name)) {
            add_diag(result.diagnostics, "XIMP0039", "COFF import archive member name must be non-empty printable ASCII");
            return result;
        }
        const auto found_library = libraries.find(library_stem);
        if (found_library == libraries.end()) {
            const auto name_owner = member_name_to_library_stem.find(member_name);
            if (name_owner != member_name_to_library_stem.end() && name_owner->second != library_stem) {
                add_diag(result.diagnostics, "XIMP0041", "different DLL groups must use different COFF archive-member names");
                return result;
            }
            member_name_to_library_stem.emplace(member_name, library_stem);
            libraries.emplace(library_stem, std::make_pair(import_symbol.dll_name, member_name));
        } else {
            if (found_library->second.first != import_symbol.dll_name) {
                add_diag(result.diagnostics, "XIMP0028", "DLL names with the same import-descriptor stem must use identical spelling");
                return result;
            }
            if (found_library->second.second != member_name) {
                add_diag(result.diagnostics, "XIMP0040", "all objects for the same DLL must use one identical COFF archive-member name");
                return result;
            }
        }
        validated_names.push_back(member_name);

        CoffShortImportObject parsed;
        parsed.machine = machine;
        parsed.timestamp = import_symbol.timestamp;
        parsed.symbol_name = import_symbol.symbol_name;
        parsed.dll_name = import_symbol.dll_name;
        parsed.type = import_symbol.type;
        parsed.name_type = import_symbol.name_type;
        parsed.ordinal_or_hint = import_symbol.ordinal_or_hint;
        std::vector<std::string> member_symbols;
        if (!append_import_symbols(parsed, all_symbols, member_symbols, result.diagnostics)) return result;
    }

    std::vector<ArchiveContainerMember> members;
    members.reserve(libraries.size() * 2U + 1U + imports.size());
    for (const auto& library : libraries) {
        auto descriptor = make_import_descriptor_member(
            library.second.first, library.first, library.second.second);
        if (!all_symbols.insert(descriptor.defined_symbols.front()).second) {
            add_diag(result.diagnostics, "XIMP0029", "import descriptor symbol collides with an imported symbol");
            return result;
        }
        members.push_back(std::move(descriptor));
    }

    // There is one final null descriptor for the combined import directory.
    // Its member name is immaterial to section-class relocation lookup, but
    // using the first DLL group name mirrors Microsoft's single-DLL LIB layout.
    auto null_descriptor = make_null_import_descriptor_member(libraries.begin()->second.second);
    if (!all_symbols.insert(null_descriptor.defined_symbols.front()).second) {
        add_diag(result.diagnostics, "XIMP0031", "reserved null import descriptor symbol collides with an imported symbol");
        return result;
    }
    members.push_back(std::move(null_descriptor));

    for (const auto& library : libraries) {
        auto null_thunk = make_null_thunk_member(library.first, library.second.second);
        if (!all_symbols.insert(null_thunk.defined_symbols.front()).second) {
            add_diag(result.diagnostics, "XIMP0033", "null thunk symbol collides with an imported symbol");
            return result;
        }
        members.push_back(std::move(null_thunk));
    }

    for (std::size_t index = 0U; index < imports.size(); ++index) {
        const auto& import_symbol = imports[index];
        CoffShortImportObject parsed;
        parsed.machine = machine;
        parsed.timestamp = import_symbol.timestamp;
        parsed.symbol_name = import_symbol.symbol_name;
        parsed.dll_name = import_symbol.dll_name;
        parsed.type = import_symbol.type;
        parsed.name_type = import_symbol.name_type;
        parsed.ordinal_or_hint = import_symbol.ordinal_or_hint;
        parsed.member_name = validated_names[index];
        ArchiveContainerMember member;
        member.name = parsed.member_name;
        member.contents = encode_short_import(import_symbol, machine);
        member.defined_symbols = coff_import_linker_symbols(parsed);
        members.push_back(std::move(member));
    }

    const auto archive = write_archive_container(ArchiveFormat::CoffLib, members, true);
    result.bytes = archive.bytes;
    result.diagnostics = archive.diagnostics;
    result.import_count = imports.size();
    return result;
}

CoffShortImportReadResult read_coff_short_import_object(
    const std::vector<std::uint8_t>& bytes) {
    CoffShortImportReadResult result;
    if (bytes.size() < ImportHeaderSize) {
        add_diag(result.diagnostics, "XIMP0011", "short-import object is smaller than the 20-byte header");
        return result;
    }

    std::uint16_t sig1 = 0U;
    std::uint16_t sig2 = 0U;
    std::uint16_t version = 0U;
    std::uint16_t machine = 0U;
    std::uint32_t timestamp = 0U;
    std::uint32_t data_size = 0U;
    std::uint16_t ordinal_or_hint = 0U;
    std::uint16_t type_info = 0U;
    if (!read_u16_le(bytes, 0U, sig1) || !read_u16_le(bytes, 2U, sig2) ||
        !read_u16_le(bytes, 4U, version) || !read_u16_le(bytes, 6U, machine) ||
        !read_u32_le(bytes, 8U, timestamp) || !read_u32_le(bytes, 12U, data_size) ||
        !read_u16_le(bytes, 16U, ordinal_or_hint) || !read_u16_le(bytes, 18U, type_info)) {
        add_diag(result.diagnostics, "XIMP0012", "short-import header fields are truncated");
        return result;
    }
    if (sig1 != 0U || sig2 != ImportSig2 || version != 0U) {
        add_diag(result.diagnostics, "XIMP0013", "short-import signature or version is invalid");
        return result;
    }
    if (machine != CoffMachineAmd64) {
        add_diag(result.diagnostics, "XIMP0014", "short-import object machine is not AMD64");
        return result;
    }
    if ((type_info & 0xffe0U) != 0U) {
        add_diag(result.diagnostics, "XIMP0015", "short-import reserved TypeInfo bits must be zero");
        return result;
    }
    const auto type_value = static_cast<std::uint16_t>(type_info & 0x0003U);
    const auto name_value = static_cast<std::uint16_t>((type_info >> 2U) & 0x0007U);
    if (type_value > static_cast<std::uint16_t>(CoffImportType::Const) || name_value >
        static_cast<std::uint16_t>(CoffImportNameType::NameUndecorate)) {
        add_diag(result.diagnostics, "XIMP0016", "short-import TypeInfo contains an unsupported import/name type");
        return result;
    }
    if (data_size < 4U || static_cast<std::uint64_t>(ImportHeaderSize) + data_size != bytes.size()) {
        add_diag(result.diagnostics, "XIMP0017", "short-import SizeOfData does not match the exact payload length");
        return result;
    }

    const std::size_t payload_end = bytes.size();
    const auto first_end_it = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(ImportHeaderSize),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(payload_end), 0U);
    if (first_end_it == bytes.begin() + static_cast<std::ptrdiff_t>(payload_end)) {
        add_diag(result.diagnostics, "XIMP0018", "short-import symbol name is not NUL terminated");
        return result;
    }
    const std::size_t first_end = static_cast<std::size_t>(first_end_it - bytes.begin());
    const std::size_t second_start = first_end + 1U;
    const auto second_end_it = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(second_start),
                                         bytes.begin() + static_cast<std::ptrdiff_t>(payload_end), 0U);
    if (second_end_it == bytes.begin() + static_cast<std::ptrdiff_t>(payload_end) ||
        second_end_it + 1 != bytes.begin() + static_cast<std::ptrdiff_t>(payload_end)) {
        add_diag(result.diagnostics, "XIMP0019", "short-import DLL name terminator or trailing payload is invalid");
        return result;
    }

    result.import_object.machine = machine;
    result.import_object.timestamp = timestamp;
    result.import_object.symbol_name.assign(
        reinterpret_cast<const char*>(bytes.data() + ImportHeaderSize), first_end - ImportHeaderSize);
    result.import_object.dll_name.assign(
        reinterpret_cast<const char*>(bytes.data() + second_start),
        static_cast<std::size_t>(second_end_it - (bytes.begin() + static_cast<std::ptrdiff_t>(second_start))));
    result.import_object.type = static_cast<CoffImportType>(type_value);
    result.import_object.name_type = static_cast<CoffImportNameType>(name_value);
    result.import_object.ordinal_or_hint = ordinal_or_hint;

    if (!valid_ascii_name(result.import_object.symbol_name) ||
        !valid_ascii_name(result.import_object.dll_name)) {
        add_diag(result.diagnostics, "XIMP0020", "short-import names must be non-empty printable ASCII");
        return result;
    }
    if (result.import_object.name_type == CoffImportNameType::Ordinal && ordinal_or_hint == 0U) {
        add_diag(result.diagnostics, "XIMP0021", "short-import ordinal must be non-zero");
        return result;
    }
    return result;
}

CoffImportLibraryReadResult read_coff_import_library(
    const std::vector<std::uint8_t>& bytes) {
    CoffImportLibraryReadResult result;
    const auto archive_result = read_archive_container(bytes);
    if (!archive_result.succeeded()) {
        result.diagnostics = archive_result.diagnostics;
        return result;
    }
    result.archive = archive_result.archive;
    if (result.archive.format != ArchiveFormat::CoffLib || !result.archive.has_symbol_index) {
        add_diag(result.diagnostics, "XIMP0022", "input is not an indexed COFF LIB archive");
        return result;
    }
    if (result.archive.members.empty()) {
        add_diag(result.diagnostics, "XIMP0023", "COFF import library contains no short-import members");
        return result;
    }

    std::map<std::string, std::string> libraries;
    std::map<std::string, std::string> library_member_names;
    std::map<std::string, std::string> member_name_to_library_stem;
    std::set<std::string> all_symbols;
    const std::size_t NoImport = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> import_index_for_member(result.archive.members.size(), NoImport);
    result.imports.reserve(result.archive.members.size());
    for (std::size_t index = 0U; index < result.archive.members.size(); ++index) {
        const auto& member = result.archive.members[index];
        if (!looks_like_short_import(member.contents)) continue;
        const auto decoded = read_coff_short_import_object(member.contents);
        if (!decoded.succeeded()) {
            result.diagnostics = decoded.diagnostics;
            return result;
        }
        auto import_object = decoded.import_object;
        import_object.member_name = member.name;
        const std::string library_stem = import_library_stem(import_object.dll_name);
        if (library_stem.empty()) {
            add_diag(result.diagnostics, "XIMP0035", "short-import DLL name has an empty descriptor stem");
            return result;
        }
        const auto found_library = libraries.find(library_stem);
        if (found_library == libraries.end()) {
            const auto name_owner = member_name_to_library_stem.find(member.name);
            if (name_owner != member_name_to_library_stem.end() && name_owner->second != library_stem) {
                add_diag(result.diagnostics, "XIMP0041", "different DLL groups must use different COFF archive-member names");
                return result;
            }
            member_name_to_library_stem.emplace(member.name, library_stem);
            libraries.emplace(library_stem, import_object.dll_name);
            library_member_names.emplace(library_stem, member.name);
        } else {
            if (found_library->second != import_object.dll_name) {
                add_diag(result.diagnostics, "XIMP0028", "DLL names with the same import-descriptor stem must use identical spelling");
                return result;
            }
            if (library_member_names.at(library_stem) != member.name) {
                add_diag(result.diagnostics, "XIMP0040", "all objects for the same DLL must share one COFF archive-member name");
                return result;
            }
        }
        std::vector<std::string> member_symbols;
        if (!append_import_symbols(import_object, all_symbols, member_symbols, result.diagnostics)) return result;
        import_index_for_member[index] = result.imports.size();
        result.imports.push_back(std::move(import_object));
    }
    if (result.imports.empty()) {
        add_diag(result.diagnostics, "XIMP0023", "COFF import library contains no short-import members");
        return result;
    }

    std::vector<ArchiveContainerMember> expected_support;
    expected_support.reserve(libraries.size() * 2U + 1U);
    for (const auto& library : libraries) {
        expected_support.push_back(make_import_descriptor_member(
            library.second, library.first, library_member_names.at(library.first)));
    }
    expected_support.push_back(make_null_import_descriptor_member(library_member_names.begin()->second));
    for (const auto& library : libraries) {
        expected_support.push_back(make_null_thunk_member(
            library.first, library_member_names.at(library.first)));
    }

    std::vector<bool> seen_support(expected_support.size(), false);
    std::vector<ArchiveSymbolIndexEntry> expected_index;
    for (std::size_t index = 0U; index < result.archive.members.size(); ++index) {
        const auto& member = result.archive.members[index];
        if (import_index_for_member[index] != NoImport) {
            const auto& import_object = result.imports[import_index_for_member[index]];
            for (const auto& symbol : coff_import_linker_symbols(import_object)) {
                expected_index.push_back(ArchiveSymbolIndexEntry{symbol, index});
            }
            continue;
        }

        std::size_t support_index = 0U;
        while (support_index < expected_support.size()) {
            if (!seen_support[support_index] &&
                expected_support[support_index].name == member.name &&
                expected_support[support_index].contents == member.contents) {
                break;
            }
            ++support_index;
        }
        if (support_index == expected_support.size()) {
            add_diag(result.diagnostics, "XIMP0037", "COFF import library contains an unknown, duplicate, or malformed descriptor support member");
            return result;
        }
        seen_support[support_index] = true;
        for (const auto& symbol : expected_support[support_index].defined_symbols) {
            expected_index.push_back(ArchiveSymbolIndexEntry{symbol, index});
        }
    }
    if (std::find(seen_support.begin(), seen_support.end(), false) != seen_support.end()) {
        add_diag(result.diagnostics, "XIMP0038", "COFF import library is missing descriptor, null-descriptor, or null-thunk support members");
        return result;
    }

    std::stable_sort(expected_index.begin(), expected_index.end(),
        [](const ArchiveSymbolIndexEntry& left, const ArchiveSymbolIndexEntry& right) {
            if (left.symbol_name != right.symbol_name) return left.symbol_name < right.symbol_name;
            return left.member_index < right.member_index;
        });
    if (expected_index.size() != result.archive.symbol_index.size()) {
        add_diag(result.diagnostics, "XIMP0024", "COFF import library linker index does not cover import and descriptor symbols exactly");
        return result;
    }
    for (std::size_t index = 0U; index < expected_index.size(); ++index) {
        if (expected_index[index].symbol_name != result.archive.symbol_index[index].symbol_name ||
            expected_index[index].member_index != result.archive.symbol_index[index].member_index) {
            add_diag(result.diagnostics, "XIMP0025", "COFF import library linker index disagrees with its import and descriptor members");
            return result;
        }
    }
    return result;
}

} // namespace xfrontend
