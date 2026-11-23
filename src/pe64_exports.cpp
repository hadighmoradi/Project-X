#include "xfrontend/pe64_exports.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xfrontend {
namespace {

constexpr std::uint16_t kMachineAmd64 = 0x8664U;
constexpr std::uint16_t kPe32PlusMagic = 0x020bU;
constexpr std::uint16_t kImageFileDll = 0x2000U;
constexpr std::size_t kOptionalHeaderMinimum = 240U;
constexpr std::size_t kExportDirectorySize = 40U;
constexpr std::uint32_t kMaximumSections = 96U;
constexpr std::uint32_t kMaximumExports = 65536U;
constexpr std::uint32_t kMaximumNames = 65536U;
constexpr std::size_t kMaximumStringBytes = 4096U;

struct PeSection {
    std::uint32_t virtual_address = 0U;
    std::uint32_t virtual_size = 0U;
    std::uint32_t raw_offset = 0U;
    std::uint32_t raw_size = 0U;
};

void add_diag(std::vector<Diagnostic>& diagnostics, const char* code,
              const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}

bool read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
    return true;
}

bool read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    return true;
}

bool read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::uint64_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 8U) return false;
    value = 0U;
    for (unsigned index = 0U; index < 8U; ++index)
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    return true;
}

bool printable_ascii(const std::string& value) {
    if (value.empty()) return false;
    for (const unsigned char ch : value) {
        if (ch < 0x20U || ch > 0x7eU) return false;
    }
    return true;
}

bool map_rva_to_file(const std::vector<std::uint8_t>& image,
                     const std::vector<PeSection>& sections,
                     std::uint32_t size_of_headers,
                     std::uint32_t rva, std::size_t count,
                     std::size_t& offset) {
    if (rva < size_of_headers) {
        const std::uint64_t end = static_cast<std::uint64_t>(rva) + count;
        if (end > size_of_headers || end > image.size()) return false;
        offset = rva;
        return true;
    }
    for (const auto& section : sections) {
        if (rva < section.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
        if (delta > section.raw_size || count > static_cast<std::uint64_t>(section.raw_size) - delta)
            continue;
        const std::uint64_t file_offset = static_cast<std::uint64_t>(section.raw_offset) + delta;
        const std::uint64_t end = file_offset + count;
        if (end > image.size()) return false;
        offset = static_cast<std::size_t>(file_offset);
        return true;
    }
    return false;
}

bool rva_is_mapped(const std::vector<PeSection>& sections,
                   std::uint32_t size_of_headers, std::uint32_t size_of_image,
                   std::uint32_t rva) {
    if (rva >= size_of_image) return false;
    if (rva < size_of_headers) return true;
    for (const auto& section : sections) {
        if (rva < section.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
        const std::uint64_t virtual_extent = std::max(section.virtual_size, section.raw_size);
        if (delta < virtual_extent) return true;
    }
    return false;
}

bool read_ascii_string(const std::vector<std::uint8_t>& image,
                       const std::vector<PeSection>& sections,
                       std::uint32_t size_of_headers,
                       std::uint32_t rva, std::size_t maximum,
                       std::string& value) {
    std::size_t offset = 0U;
    if (!map_rva_to_file(image, sections, size_of_headers, rva, 1U, offset)) return false;
    std::size_t contiguous = 0U;
    if (rva < size_of_headers) {
        contiguous = std::min<std::size_t>(size_of_headers - rva, image.size() - offset);
    } else {
        for (const auto& section : sections) {
            if (rva < section.virtual_address) continue;
            const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
            if (delta < section.raw_size) {
                contiguous = std::min<std::size_t>(static_cast<std::size_t>(section.raw_size - delta),
                                                   image.size() - offset);
                break;
            }
        }
    }
    if (contiguous == 0U) return false;
    const std::size_t available = std::min(maximum, contiguous);
    value.clear();
    for (std::size_t index = 0U; index < available; ++index) {
        const auto byte = image[offset + index];
        if (byte == 0U) return printable_ascii(value);
        if (byte < 0x20U || byte > 0x7eU) return false;
        value.push_back(static_cast<char>(byte));
    }
    return false;
}

} // namespace

Pe64ExportReadResult read_pe64_export_directory(const std::vector<std::uint8_t>& image) {
    Pe64ExportReadResult result;
    if (image.size() < 0x40U) {
        add_diag(result.diagnostics, "XPEEXP0001", "Image is too small to contain a DOS header.");
        return result;
    }
    if (image[0U] != 'M' || image[1U] != 'Z') {
        add_diag(result.diagnostics, "XPEEXP0002", "DOS signature is not MZ.");
        return result;
    }
    std::uint32_t pe_offset32 = 0U;
    if (!read_u32(image, 0x3cU, pe_offset32)) {
        add_diag(result.diagnostics, "XPEEXP0003", "DOS e_lfanew field is truncated.");
        return result;
    }
    const std::size_t pe_offset = pe_offset32;
    if (pe_offset > image.size() || image.size() - pe_offset < 24U) {
        add_diag(result.diagnostics, "XPEEXP0004", "PE signature or COFF header is truncated.");
        return result;
    }
    if (image[pe_offset] != 'P' || image[pe_offset + 1U] != 'E' ||
        image[pe_offset + 2U] != 0U || image[pe_offset + 3U] != 0U) {
        add_diag(result.diagnostics, "XPEEXP0005", "PE signature is invalid.");
        return result;
    }

    std::uint16_t machine = 0U;
    std::uint16_t section_count = 0U;
    std::uint16_t optional_size16 = 0U;
    std::uint16_t characteristics = 0U;
    if (!read_u16(image, pe_offset + 4U, machine) ||
        !read_u16(image, pe_offset + 6U, section_count) ||
        !read_u16(image, pe_offset + 20U, optional_size16) ||
        !read_u16(image, pe_offset + 22U, characteristics)) {
        add_diag(result.diagnostics, "XPEEXP0006", "COFF header fields are truncated.");
        return result;
    }
    if (machine != kMachineAmd64) {
        add_diag(result.diagnostics, "XPEEXP0007", "Only AMD64 PE images are supported.");
        return result;
    }
    if ((characteristics & kImageFileDll) == 0U) {
        add_diag(result.diagnostics, "XPEEXP0008", "PE image is not marked as a DLL.");
        return result;
    }
    if (section_count == 0U || section_count > kMaximumSections) {
        add_diag(result.diagnostics, "XPEEXP0009", "Section count is outside the supported range.");
        return result;
    }

    const std::size_t optional_offset = pe_offset + 24U;
    const std::size_t optional_size = optional_size16;
    if (optional_size < kOptionalHeaderMinimum || optional_offset > image.size() ||
        optional_size > image.size() - optional_offset) {
        add_diag(result.diagnostics, "XPEEXP0010", "PE32+ optional header is missing or truncated.");
        return result;
    }
    std::uint16_t optional_magic = 0U;
    std::uint64_t image_base = 0U;
    std::uint32_t size_of_image = 0U;
    std::uint32_t size_of_headers = 0U;
    std::uint32_t number_of_directories = 0U;
    if (!read_u16(image, optional_offset, optional_magic) ||
        !read_u64(image, optional_offset + 24U, image_base) ||
        !read_u32(image, optional_offset + 56U, size_of_image) ||
        !read_u32(image, optional_offset + 60U, size_of_headers) ||
        !read_u32(image, optional_offset + 108U, number_of_directories)) {
        add_diag(result.diagnostics, "XPEEXP0011", "PE32+ optional-header fields are truncated.");
        return result;
    }
    if (optional_magic != kPe32PlusMagic) {
        add_diag(result.diagnostics, "XPEEXP0012", "Optional-header magic is not PE32+.");
        return result;
    }
    if (number_of_directories == 0U || optional_size < 120U) {
        add_diag(result.diagnostics, "XPEEXP0013", "Export data-directory entry is not present.");
        return result;
    }
    const std::uint64_t available_directories = (optional_size - 112U) / 8U;
    if (number_of_directories > available_directories) {
        add_diag(result.diagnostics, "XPEEXP0014", "NumberOfRvaAndSizes exceeds optional-header bounds.");
        return result;
    }
    if (size_of_image == 0U || size_of_headers == 0U || size_of_headers > image.size() ||
        size_of_headers > size_of_image) {
        add_diag(result.diagnostics, "XPEEXP0015", "SizeOfImage or SizeOfHeaders is invalid.");
        return result;
    }

    std::uint32_t export_rva = 0U;
    std::uint32_t export_size = 0U;
    if (!read_u32(image, optional_offset + 112U, export_rva) ||
        !read_u32(image, optional_offset + 116U, export_size) ||
        export_rva == 0U || export_size < kExportDirectorySize) {
        add_diag(result.diagnostics, "XPEEXP0016", "Export data directory is absent or too small.");
        return result;
    }
    const std::uint64_t export_end64 = static_cast<std::uint64_t>(export_rva) + export_size;
    if (export_end64 > size_of_image || export_end64 > std::numeric_limits<std::uint32_t>::max()) {
        add_diag(result.diagnostics, "XPEEXP0017", "Export data directory exceeds the image RVA range.");
        return result;
    }
    const auto export_end = static_cast<std::uint32_t>(export_end64);

    const std::size_t section_table = optional_offset + optional_size;
    const std::uint64_t section_table_size = static_cast<std::uint64_t>(section_count) * 40U;
    if (section_table > image.size() || section_table_size > image.size() - section_table ||
        section_table + section_table_size > size_of_headers) {
        add_diag(result.diagnostics, "XPEEXP0018", "Section table exceeds file or header bounds.");
        return result;
    }
    std::vector<PeSection> sections;
    sections.reserve(section_count);
    for (std::uint16_t index = 0U; index < section_count; ++index) {
        const std::size_t offset = section_table + static_cast<std::size_t>(index) * 40U;
        PeSection section;
        if (!read_u32(image, offset + 8U, section.virtual_size) ||
            !read_u32(image, offset + 12U, section.virtual_address) ||
            !read_u32(image, offset + 16U, section.raw_size) ||
            !read_u32(image, offset + 20U, section.raw_offset)) {
            add_diag(result.diagnostics, "XPEEXP0019", "Section header is truncated.");
            return result;
        }
        const std::uint64_t raw_end = static_cast<std::uint64_t>(section.raw_offset) + section.raw_size;
        const std::uint64_t virtual_end = static_cast<std::uint64_t>(section.virtual_address) +
            std::max(section.virtual_size, section.raw_size);
        if (section.raw_size != 0U && (section.raw_offset < size_of_headers || raw_end > image.size())) {
            add_diag(result.diagnostics, "XPEEXP0020", "Section raw data exceeds file bounds.");
            return result;
        }
        if (section.virtual_address < size_of_headers || virtual_end > size_of_image ||
            virtual_end > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(result.diagnostics, "XPEEXP0021", "Section virtual range exceeds image bounds.");
            return result;
        }
        sections.push_back(section);
    }
    for (std::size_t left = 0U; left < sections.size(); ++left) {
        const std::uint64_t left_start = sections[left].virtual_address;
        const std::uint64_t left_end = left_start + std::max(sections[left].virtual_size, sections[left].raw_size);
        for (std::size_t right = left + 1U; right < sections.size(); ++right) {
            const std::uint64_t right_start = sections[right].virtual_address;
            const std::uint64_t right_end = right_start + std::max(sections[right].virtual_size, sections[right].raw_size);
            if (left_start < right_end && right_start < left_end) {
                add_diag(result.diagnostics, "XPEEXP0022", "Section virtual ranges overlap.");
                return result;
            }
        }
    }

    std::size_t directory_offset = 0U;
    if (!map_rva_to_file(image, sections, size_of_headers, export_rva,
                         kExportDirectorySize, directory_offset)) {
        add_diag(result.diagnostics, "XPEEXP0023", "Export directory RVA is not fully backed by file data.");
        return result;
    }
    std::uint32_t dll_name_rva = 0U;
    std::uint32_t ordinal_base = 0U;
    std::uint32_t function_count = 0U;
    std::uint32_t name_count = 0U;
    std::uint32_t functions_rva = 0U;
    std::uint32_t names_rva = 0U;
    std::uint32_t ordinals_rva = 0U;
    if (!read_u32(image, directory_offset + 12U, dll_name_rva) ||
        !read_u32(image, directory_offset + 16U, ordinal_base) ||
        !read_u32(image, directory_offset + 20U, function_count) ||
        !read_u32(image, directory_offset + 24U, name_count) ||
        !read_u32(image, directory_offset + 28U, functions_rva) ||
        !read_u32(image, directory_offset + 32U, names_rva) ||
        !read_u32(image, directory_offset + 36U, ordinals_rva)) {
        add_diag(result.diagnostics, "XPEEXP0024", "Export directory fields are truncated.");
        return result;
    }
    if (ordinal_base == 0U || function_count == 0U || function_count > kMaximumExports ||
        name_count > kMaximumNames || (name_count > 0U && (names_rva == 0U || ordinals_rva == 0U))) {
        add_diag(result.diagnostics, "XPEEXP0025", "Export ordinal base or function/name count is invalid.");
        return result;
    }
    std::string dll_name;
    if (!read_ascii_string(image, sections, size_of_headers, dll_name_rva,
                           kMaximumStringBytes, dll_name)) {
        add_diag(result.diagnostics, "XPEEXP0026", "Export DLL name is invalid or unterminated.");
        return result;
    }

    const std::uint64_t function_bytes64 = static_cast<std::uint64_t>(function_count) * 4U;
    const std::uint64_t name_bytes64 = static_cast<std::uint64_t>(name_count) * 4U;
    const std::uint64_t ordinal_bytes64 = static_cast<std::uint64_t>(name_count) * 2U;
    if (function_bytes64 > std::numeric_limits<std::size_t>::max() ||
        name_bytes64 > std::numeric_limits<std::size_t>::max() ||
        ordinal_bytes64 > std::numeric_limits<std::size_t>::max()) {
        add_diag(result.diagnostics, "XPEEXP0027", "Export table array length overflows host size.");
        return result;
    }
    std::size_t functions_offset = 0U;
    if (!map_rva_to_file(image, sections, size_of_headers, functions_rva,
                         static_cast<std::size_t>(function_bytes64), functions_offset)) {
        add_diag(result.diagnostics, "XPEEXP0028", "Export Address Table is outside file-backed section data.");
        return result;
    }
    std::size_t names_offset = 0U;
    std::size_t ordinals_offset = 0U;
    if (name_count > 0U &&
        (!map_rva_to_file(image, sections, size_of_headers, names_rva,
                          static_cast<std::size_t>(name_bytes64), names_offset) ||
         !map_rva_to_file(image, sections, size_of_headers, ordinals_rva,
                          static_cast<std::size_t>(ordinal_bytes64), ordinals_offset))) {
        add_diag(result.diagnostics, "XPEEXP0029", "Export name or ordinal table is outside file-backed section data.");
        return result;
    }

    std::vector<std::vector<std::pair<std::string, std::uint16_t>>> names_by_function(function_count);
    std::set<std::string> unique_names;
    for (std::uint32_t index = 0U; index < name_count; ++index) {
        std::uint32_t name_rva = 0U;
        std::uint16_t function_index = 0U;
        if (!read_u32(image, names_offset + static_cast<std::size_t>(index) * 4U, name_rva) ||
            !read_u16(image, ordinals_offset + static_cast<std::size_t>(index) * 2U, function_index)) {
            add_diag(result.diagnostics, "XPEEXP0030", "Export name pointer or ordinal table is truncated.");
            return result;
        }
        if (function_index >= function_count) {
            add_diag(result.diagnostics, "XPEEXP0031", "Export name ordinal index is outside the function table.");
            return result;
        }
        std::string export_name;
        if (!read_ascii_string(image, sections, size_of_headers, name_rva,
                               kMaximumStringBytes, export_name)) {
            add_diag(result.diagnostics, "XPEEXP0032", "Export name is invalid or unterminated.");
            return result;
        }
        if (!unique_names.insert(export_name).second) {
            add_diag(result.diagnostics, "XPEEXP0033", "Export name table contains a duplicate name.");
            return result;
        }
        names_by_function[function_index].push_back({std::move(export_name),
            static_cast<std::uint16_t>(index <= 0xffffU ? index : 0U)});
    }

    result.directory.dll_name = std::move(dll_name);
    result.directory.image_base = image_base;
    result.directory.directory_rva = export_rva;
    result.directory.directory_size = export_size;
    result.directory.ordinal_base = ordinal_base;
    for (std::uint32_t index = 0U; index < function_count; ++index) {
        std::uint32_t function_rva = 0U;
        if (!read_u32(image, functions_offset + static_cast<std::size_t>(index) * 4U, function_rva)) {
            add_diag(result.diagnostics, "XPEEXP0034", "Export Address Table is truncated.");
            return result;
        }
        if (function_rva == 0U) continue; // A reserved ordinal slot with no export.
        const std::uint64_t ordinal64 = static_cast<std::uint64_t>(ordinal_base) + index;
        if (ordinal64 > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(result.diagnostics, "XPEEXP0035", "Export ordinal overflows 32 bits.");
            return result;
        }
        const bool forwarded = function_rva >= export_rva && function_rva < export_end;
        std::string forwarder;
        if (forwarded) {
            const std::size_t remaining = static_cast<std::size_t>(export_end - function_rva);
            if (!read_ascii_string(image, sections, size_of_headers, function_rva,
                                   std::min(remaining, kMaximumStringBytes), forwarder)) {
                add_diag(result.diagnostics, "XPEEXP0036", "Forwarder string is invalid or not terminated inside the export directory.");
                return result;
            }
        } else if (!rva_is_mapped(sections, size_of_headers, size_of_image, function_rva)) {
            add_diag(result.diagnostics, "XPEEXP0037", "Export address does not point into a mapped image range.");
            return result;
        }

        if (names_by_function[index].empty()) {
            Pe64ExportSymbol symbol;
            symbol.ordinal = static_cast<std::uint32_t>(ordinal64);
            symbol.function_rva = function_rva;
            symbol.forwarded = forwarded;
            symbol.forwarder = std::move(forwarder);
            result.directory.exports.push_back(std::move(symbol));
        } else {
            for (auto& named : names_by_function[index]) {
                Pe64ExportSymbol symbol;
                symbol.name = std::move(named.first);
                symbol.ordinal = static_cast<std::uint32_t>(ordinal64);
                symbol.hint = named.second;
                symbol.function_rva = function_rva;
                symbol.forwarded = forwarded;
                symbol.forwarder = forwarder;
                result.directory.exports.push_back(std::move(symbol));
            }
        }
    }
    if (result.directory.exports.empty()) {
        add_diag(result.diagnostics, "XPEEXP0038", "Export Directory contains no populated export address slots.");
    }
    return result;
}

Pe64ExportImportLibraryResult write_coff_import_library_from_pe64_dll(
    const std::vector<std::uint8_t>& image,
    const std::vector<Pe64ExportKindOverride>& kind_overrides) {
    Pe64ExportImportLibraryResult result;
    const auto parsed = read_pe64_export_directory(image);
    if (!parsed.succeeded()) {
        result.diagnostics = parsed.diagnostics;
        return result;
    }

    std::map<std::string, CoffImportType> named_overrides;
    std::map<std::uint32_t, CoffImportType> ordinal_overrides;
    for (const auto& item : kind_overrides) {
        if (item.export_name.empty() == (item.ordinal == 0U)) {
            add_diag(result.diagnostics, "XPEEXP0040", "An export-kind override must specify exactly one of export_name or ordinal.");
            return result;
        }
        if (item.type != CoffImportType::Code && item.type != CoffImportType::Data &&
            item.type != CoffImportType::Const) {
            add_diag(result.diagnostics, "XPEEXP0041", "Export-kind override contains an unsupported COFF import type.");
            return result;
        }
        if (!item.export_name.empty()) {
            if (!printable_ascii(item.export_name) ||
                !named_overrides.emplace(item.export_name, item.type).second) {
                add_diag(result.diagnostics, "XPEEXP0042", "Named export-kind override is invalid or duplicated.");
                return result;
            }
        } else if (!ordinal_overrides.emplace(item.ordinal, item.type).second) {
            add_diag(result.diagnostics, "XPEEXP0043", "Ordinal export-kind override is duplicated.");
            return result;
        }
    }

    std::set<std::string> seen_override_names;
    std::set<std::uint32_t> seen_override_ordinals;
    std::vector<CoffImportSymbol> imports;
    imports.reserve(parsed.directory.exports.size());
    for (const auto& export_symbol : parsed.directory.exports) {
        CoffImportSymbol import;
        import.dll_name = parsed.directory.dll_name;
        import.type = CoffImportType::Code;
        const auto ordinal_override = ordinal_overrides.find(export_symbol.ordinal);
        const auto named_override = export_symbol.name.empty()
            ? named_overrides.end() : named_overrides.find(export_symbol.name);
        if (ordinal_override != ordinal_overrides.end() && named_override != named_overrides.end()) {
            add_diag(result.diagnostics, "XPEEXP0047", "An export is matched by both name and ordinal overrides; use one override key.");
            return result;
        }
        if (ordinal_override != ordinal_overrides.end()) {
            import.type = ordinal_override->second;
            seen_override_ordinals.insert(ordinal_override->first);
        }
        if (!export_symbol.name.empty()) {
            import.symbol_name = export_symbol.name;
            import.name_type = CoffImportNameType::Name;
            import.ordinal_or_hint = export_symbol.hint;
            if (named_override != named_overrides.end()) {
                import.type = named_override->second;
                seen_override_names.insert(named_override->first);
            }
        } else {
            if (export_symbol.ordinal == 0U || export_symbol.ordinal > 0xffffU) {
                add_diag(result.diagnostics, "XPEEXP0044", "Ordinal-only export cannot be represented by the AMD64 short-import header.");
                return result;
            }
            // Short-import payloads retain a non-empty linker-facing symbol
            // name even when the loader lookup is by ordinal. This stable
            // synthetic spelling is the source-level name exposed by the LIB.
            import.symbol_name = "xfrontend_ordinal_" + std::to_string(export_symbol.ordinal);
            import.name_type = CoffImportNameType::Ordinal;
            import.ordinal_or_hint = static_cast<std::uint16_t>(export_symbol.ordinal);
        }
        imports.push_back(std::move(import));
    }
    for (const auto& item : named_overrides) {
        if (seen_override_names.find(item.first) == seen_override_names.end()) {
            add_diag(result.diagnostics, "XPEEXP0045", "Named export-kind override does not match an exported name: " + item.first);
        }
    }
    for (const auto& item : ordinal_overrides) {
        const bool exists = std::any_of(parsed.directory.exports.begin(), parsed.directory.exports.end(),
            [&item](const Pe64ExportSymbol& symbol) { return symbol.ordinal == item.first; });
        if (!exists || seen_override_ordinals.find(item.first) == seen_override_ordinals.end()) {
            add_diag(result.diagnostics, "XPEEXP0046", "Ordinal export-kind override does not match an ordinal-only export.");
        }
    }
    if (!result.diagnostics.empty()) return result;

    const auto written = write_coff_import_library(imports, CoffMachineAmd64);
    if (!written.succeeded()) {
        result.diagnostics = written.diagnostics;
        return result;
    }
    result.bytes = written.bytes;
    result.import_count = imports.size();
    return result;
}

} // namespace xfrontend
