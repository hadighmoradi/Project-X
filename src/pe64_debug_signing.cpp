#include "xfrontend/pe64_debug_signing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace xfrontend {
namespace {

constexpr std::size_t kDosPeOffset = 0x3cU;
constexpr std::size_t kCoffHeaderSize = 20U;
constexpr std::size_t kPe32PlusOptionalHeaderSize = 240U;
constexpr std::size_t kSectionHeaderSize = 40U;
constexpr std::size_t kDataDirectoryOffset = 112U;
constexpr std::size_t kNumberOfRvaAndSizesOffset = 108U;
constexpr std::size_t kDebugDirectoryIndex = 6U;
constexpr std::size_t kSecurityDirectoryIndex = 4U;
constexpr std::uint32_t kInitializedReadOnly = 0x40000040U;
constexpr std::uint32_t kMaximumDebugBytes = 1024U * 1024U;
constexpr std::uint32_t kMaximumFileBytes = 512U * 1024U * 1024U;
constexpr std::uint16_t kWinCertRevision10 = 0x0100U;
constexpr std::uint16_t kWinCertRevision20 = 0x0200U;
constexpr std::uint16_t kWinCertTypePkcsSignedData = 0x0002U;

void add_error(std::vector<Diagnostic>& diagnostics, const char* code, const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, SourceSpan{}});
}

bool range_fits(std::size_t size, std::size_t offset, std::size_t length) {
    return offset <= size && length <= size - offset;
}

std::uint16_t read16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

bool align_up(std::uint64_t value, std::uint32_t alignment, std::uint64_t& output) {
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) return false;
    const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1U);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) return false;
    output = (value + mask) & ~mask;
    return true;
}

struct PeLayout {
    std::size_t pe_offset = 0U;
    std::size_t optional_offset = 0U;
    std::size_t optional_size = 0U;
    std::size_t section_table = 0U;
    std::uint16_t section_count = 0U;
    std::uint32_t size_headers = 0U;
    std::uint32_t section_alignment = 0U;
    std::uint32_t file_alignment = 0U;
};

bool read_layout(const std::vector<std::uint8_t>& bytes, PeLayout& layout, std::string& error) {
    if (!range_fits(bytes.size(), 0U, 0x40U) || bytes[0] != 'M' || bytes[1] != 'Z') {
        error = "Input is not a valid DOS/PE image.";
        return false;
    }
    const std::uint32_t pe32_offset = read32(bytes, kDosPeOffset);
    const std::size_t pe = static_cast<std::size_t>(pe32_offset);
    if (!range_fits(bytes.size(), pe, 4U + kCoffHeaderSize) || bytes[pe] != 'P' ||
        bytes[pe + 1U] != 'E' || bytes[pe + 2U] != 0U || bytes[pe + 3U] != 0U) {
        error = "PE signature or COFF header is truncated.";
        return false;
    }
    const std::size_t optional = pe + 4U + kCoffHeaderSize;
    const std::uint16_t optional_size = read16(bytes, pe + 20U);
    const std::uint16_t section_count = read16(bytes, pe + 6U);
    if (optional_size < kPe32PlusOptionalHeaderSize ||
        !range_fits(bytes.size(), optional, optional_size) || read16(bytes, optional) != 0x20bU) {
        error = "PE32+ optional header is missing or truncated.";
        return false;
    }
    const std::uint32_t number_directories = read32(bytes, optional + kNumberOfRvaAndSizesOffset);
    if (number_directories < 16U || optional_size < kPe32PlusOptionalHeaderSize) {
        error = "PE32+ optional header does not expose the standard data directories.";
        return false;
    }
    const std::uint32_t size_headers = read32(bytes, optional + 60U);
    const std::size_t section_table = optional + optional_size;
    const std::uint64_t section_bytes = static_cast<std::uint64_t>(section_count) * kSectionHeaderSize;
    if (size_headers > bytes.size() || section_table > size_headers ||
        section_bytes > static_cast<std::uint64_t>(size_headers - section_table)) {
        error = "PE section table is outside SizeOfHeaders.";
        return false;
    }
    layout.pe_offset = pe;
    layout.optional_offset = optional;
    layout.optional_size = optional_size;
    layout.section_table = section_table;
    layout.section_count = section_count;
    layout.size_headers = size_headers;
    layout.section_alignment = read32(bytes, optional + 32U);
    layout.file_alignment = read32(bytes, optional + 36U);
    if (layout.section_alignment == 0U || layout.file_alignment == 0U ||
        (layout.section_alignment & (layout.section_alignment - 1U)) != 0U ||
        (layout.file_alignment & (layout.file_alignment - 1U)) != 0U) {
        error = "PE section or file alignment is invalid.";
        return false;
    }
    return true;
}

std::size_t rva_to_offset(const std::vector<std::uint8_t>& bytes, const PeLayout& layout,
                          std::uint32_t rva, std::size_t length) {
    for (std::size_t i = 0U; i < layout.section_count; ++i) {
        const std::size_t section = layout.section_table + i * kSectionHeaderSize;
        const std::uint32_t virtual_size = read32(bytes, section + 8U);
        const std::uint32_t virtual_address = read32(bytes, section + 12U);
        const std::uint32_t raw_size = read32(bytes, section + 16U);
        const std::uint32_t raw_offset = read32(bytes, section + 20U);
        if (rva < virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - virtual_address;
        const std::uint64_t span = std::max(virtual_size, raw_size);
        if (delta > span || length > span - delta || delta > raw_size || length > raw_size - delta) continue;
        const std::uint64_t offset = static_cast<std::uint64_t>(raw_offset) + delta;
        if (offset > bytes.size() || length > bytes.size() - static_cast<std::size_t>(offset)) continue;
        return static_cast<std::size_t>(offset);
    }
    return std::numeric_limits<std::size_t>::max();
}

} // namespace

Pe64DebugResult write_pe64_codeview_debug_directory(
    const Pe64DllImage& input, const Pe64CodeViewSpec& spec) {
    Pe64DebugResult result;
    result.image = input;
    if (spec.age == 0U) {
        add_error(result.diagnostics, "XPE64DBG0001", "CodeView PDB age must be greater than zero.");
        return result;
    }
    if (spec.pdb_path.empty() || spec.pdb_path.size() > 1024U ||
        spec.pdb_path.find('\0') != std::string::npos ||
        !std::all_of(spec.pdb_path.begin(), spec.pdb_path.end(), [](unsigned char c) {
            return c >= 0x20U && c <= 0x7eU;
        })) {
        add_error(result.diagnostics, "XPE64DBG0002", "CodeView PDB path must be 1..1024 printable ASCII bytes without an embedded NUL.");
        return result;
    }
    PeLayout layout;
    std::string error;
    if (!read_layout(input.bytes, layout, error)) {
        add_error(result.diagnostics, "XPE64DBG0003", error);
        return result;
    }
    const std::size_t debug_directory = layout.optional_offset + kDataDirectoryOffset + kDebugDirectoryIndex * 8U;
    if (read32(input.bytes, debug_directory) != 0U || read32(input.bytes, debug_directory + 4U) != 0U) {
        add_error(result.diagnostics, "XPE64DBG0004", "Input PE image already has a Debug Directory.");
        return result;
    }
    if (layout.section_count >= 96U || layout.section_count == std::numeric_limits<std::uint16_t>::max()) {
        add_error(result.diagnostics, "XPE64DBG0005", "PE section count has reached the Windows loader limit.");
        return result;
    }
    const std::uint64_t header_slot = static_cast<std::uint64_t>(layout.section_table) +
        static_cast<std::uint64_t>(layout.section_count) * kSectionHeaderSize;
    if (header_slot > layout.size_headers || layout.size_headers - header_slot < kSectionHeaderSize) {
        add_error(result.diagnostics, "XPE64DBG0006", "PE headers have no room for the additional .debug section header.");
        return result;
    }
    const std::uint64_t section_header = static_cast<std::uint64_t>(layout.section_table) +
        static_cast<std::uint64_t>(layout.section_count) * kSectionHeaderSize;
    for (std::size_t i = 0U; i < layout.section_count; ++i) {
        const std::size_t current = layout.section_table + i * kSectionHeaderSize;
        if (input.bytes[current] == '.' && input.bytes[current + 1U] == 'd' &&
            input.bytes[current + 2U] == 'e' && input.bytes[current + 3U] == 'b' &&
            input.bytes[current + 4U] == 'u' && input.bytes[current + 5U] == 'g') {
            add_error(result.diagnostics, "XPE64DBG0007", "A .debug section already exists in the input image.");
            return result;
        }
    }

    const std::uint64_t codeview_size64 = 24U + spec.pdb_path.size() + 1U;
    const std::uint64_t payload_size64 = 28U + codeview_size64;
    if (payload_size64 > kMaximumDebugBytes || payload_size64 > std::numeric_limits<std::uint32_t>::max()) {
        add_error(result.diagnostics, "XPE64DBG0008", "CodeView debug payload exceeds the configured safety limit.");
        return result;
    }
    std::uint64_t section_end = 0U;
    for (std::size_t i = 0U; i < layout.section_count; ++i) {
        const std::size_t current = layout.section_table + i * kSectionHeaderSize;
        const std::uint32_t virtual_size = read32(input.bytes, current + 8U);
        const std::uint32_t virtual_address = read32(input.bytes, current + 12U);
        const std::uint32_t raw_size = read32(input.bytes, current + 16U);
        section_end = std::max(section_end, static_cast<std::uint64_t>(virtual_address) +
            std::max(virtual_size, raw_size));
    }
    std::uint64_t section_rva64 = 0U;
    if (!align_up(section_end, layout.section_alignment, section_rva64) ||
        section_rva64 > std::numeric_limits<std::uint32_t>::max()) {
        add_error(result.diagnostics, "XPE64DBG0009", "Debug section RVA exceeds PE32 addressability.");
        return result;
    }
    std::uint64_t raw_offset64 = 0U;
    std::uint64_t raw_size64 = 0U;
    if (!align_up(input.bytes.size(), layout.file_alignment, raw_offset64) ||
        !align_up(payload_size64, layout.file_alignment, raw_size64) ||
        raw_offset64 > std::numeric_limits<std::uint32_t>::max() ||
        raw_size64 > std::numeric_limits<std::uint32_t>::max() ||
        raw_offset64 + raw_size64 > kMaximumFileBytes ||
        raw_offset64 + raw_size64 > std::numeric_limits<std::size_t>::max()) {
        add_error(result.diagnostics, "XPE64DBG0010", "Debug section raw layout exceeds the file-size limit.");
        return result;
    }
    std::uint64_t image_size64 = 0U;
    if (!align_up(section_rva64 + payload_size64, layout.section_alignment, image_size64) ||
        image_size64 > std::numeric_limits<std::uint32_t>::max()) {
        add_error(result.diagnostics, "XPE64DBG0011", "Debug section increases SizeOfImage beyond PE32 addressability.");
        return result;
    }
    const std::uint64_t initialized_size64 = static_cast<std::uint64_t>(read32(input.bytes, layout.optional_offset + 8U)) + raw_size64;
    if (initialized_size64 > std::numeric_limits<std::uint32_t>::max()) {
        add_error(result.diagnostics, "XPE64DBG0012", "SizeOfInitializedData overflows after adding .debug.");
        return result;
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_size64), 0U);
    const std::size_t codeview_offset = 28U;
    payload[codeview_offset] = 'R';
    payload[codeview_offset + 1U] = 'S';
    payload[codeview_offset + 2U] = 'D';
    payload[codeview_offset + 3U] = 'S';
    std::copy(spec.guid.begin(), spec.guid.end(), payload.begin() + static_cast<std::ptrdiff_t>(codeview_offset + 4U));
    write32(payload, codeview_offset + 20U, spec.age);
    std::copy(spec.pdb_path.begin(), spec.pdb_path.end(),
        payload.begin() + static_cast<std::ptrdiff_t>(codeview_offset + 24U));
    payload.back() = 0U;

    // IMAGE_DEBUG_DIRECTORY is seven 32-bit/16-bit fields (28 bytes).
    write32(payload, 4U, spec.timestamp);
    write16(payload, 8U, 0U);
    write16(payload, 10U, 0U);
    write32(payload, 12U, 2U); // IMAGE_DEBUG_TYPE_CODEVIEW
    write32(payload, 16U, static_cast<std::uint32_t>(codeview_size64));
    write32(payload, 20U, static_cast<std::uint32_t>(section_rva64 + codeview_offset));
    write32(payload, 24U, static_cast<std::uint32_t>(raw_offset64 + codeview_offset));

    const std::size_t final_size = static_cast<std::size_t>(raw_offset64 + raw_size64);
    result.image.bytes.resize(final_size, 0U);
    std::copy(payload.begin(), payload.end(),
        result.image.bytes.begin() + static_cast<std::ptrdiff_t>(raw_offset64));

    const std::size_t h = static_cast<std::size_t>(section_header);
    std::fill(result.image.bytes.begin() + static_cast<std::ptrdiff_t>(h),
              result.image.bytes.begin() + static_cast<std::ptrdiff_t>(h + kSectionHeaderSize),
              std::uint8_t{0U});
    const char section_name[] = ".debug";
    std::copy(section_name, section_name + 6,
        result.image.bytes.begin() + static_cast<std::ptrdiff_t>(h));
    write32(result.image.bytes, h + 8U, static_cast<std::uint32_t>(payload_size64));
    write32(result.image.bytes, h + 12U, static_cast<std::uint32_t>(section_rva64));
    write32(result.image.bytes, h + 16U, static_cast<std::uint32_t>(raw_size64));
    write32(result.image.bytes, h + 20U, static_cast<std::uint32_t>(raw_offset64));
    write32(result.image.bytes, h + 36U, kInitializedReadOnly);
    write16(result.image.bytes, layout.pe_offset + 6U, static_cast<std::uint16_t>(layout.section_count + 1U));
    write32(result.image.bytes, layout.optional_offset + 8U, static_cast<std::uint32_t>(initialized_size64));
    write32(result.image.bytes, layout.optional_offset + 56U, static_cast<std::uint32_t>(image_size64));
    write32(result.image.bytes, debug_directory, static_cast<std::uint32_t>(section_rva64));
    write32(result.image.bytes, debug_directory + 4U, 28U);

    result.image.sections.push_back(Pe64DllSectionInfo{".debug", static_cast<std::uint32_t>(section_rva64),
        static_cast<std::uint32_t>(payload_size64), static_cast<std::uint32_t>(raw_offset64),
        static_cast<std::uint32_t>(raw_size64), kInitializedReadOnly});
    result.image.size_of_image = static_cast<std::uint32_t>(image_size64);
    result.debug.directory_rva = static_cast<std::uint32_t>(section_rva64);
    result.debug.directory_size = 28U;
    result.debug.codeview_rva = static_cast<std::uint32_t>(section_rva64 + codeview_offset);
    result.debug.codeview_size = static_cast<std::uint32_t>(codeview_size64);
    result.debug.codeview_file_offset = static_cast<std::uint32_t>(raw_offset64 + codeview_offset);
    result.debug.timestamp = spec.timestamp;
    return result;
}

Pe64CodeViewInspection inspect_pe64_codeview_debug_directory(
    const std::vector<std::uint8_t>& image) {
    Pe64CodeViewInspection result;
    PeLayout layout;
    if (!read_layout(image, layout, result.error)) return result;
    const std::size_t entry = layout.optional_offset + kDataDirectoryOffset + kDebugDirectoryIndex * 8U;
    result.directory_rva = read32(image, entry);
    result.directory_size = read32(image, entry + 4U);
    result.present = result.directory_rva != 0U || result.directory_size != 0U;
    if (!result.present) {
        result.error = "PE Debug Directory is absent.";
        return result;
    }
    if (result.directory_rva == 0U || result.directory_size < 28U || result.directory_size % 28U != 0U) {
        result.error = "PE Debug Directory RVA or size is invalid.";
        return result;
    }
    const std::size_t directory_offset = rva_to_offset(image, layout, result.directory_rva, result.directory_size);
    if (directory_offset == std::numeric_limits<std::size_t>::max()) {
        result.error = "PE Debug Directory RVA does not map to a complete raw section range.";
        return result;
    }
    bool found_codeview = false;
    for (std::size_t offset = directory_offset; offset < directory_offset + result.directory_size; offset += 28U) {
        const std::uint32_t type = read32(image, offset + 12U);
        if (type != 2U) continue;
        if (found_codeview) {
            result.error = "Multiple CodeView debug entries are not supported by this inspector.";
            return result;
        }
        found_codeview = true;
        result.timestamp = read32(image, offset + 4U);
        result.codeview_size = read32(image, offset + 16U);
        result.codeview_rva = read32(image, offset + 20U);
        result.codeview_file_offset = read32(image, offset + 24U);
        if (result.codeview_size < 25U || result.codeview_size > kMaximumDebugBytes ||
            result.codeview_rva == 0U || result.codeview_file_offset > image.size() ||
            result.codeview_size > image.size() - result.codeview_file_offset) {
            result.error = "CodeView data size or file pointer is invalid.";
            return result;
        }
        const std::size_t mapped = rva_to_offset(image, layout, result.codeview_rva, result.codeview_size);
        if (mapped == std::numeric_limits<std::size_t>::max() || mapped != result.codeview_file_offset) {
            result.error = "CodeView AddressOfRawData and PointerToRawData do not identify the same bytes.";
            return result;
        }
        const std::size_t cv = result.codeview_file_offset;
        if (image[cv] != 'R' || image[cv + 1U] != 'S' || image[cv + 2U] != 'D' || image[cv + 3U] != 'S') {
            result.error = "CodeView record does not contain the RSDS signature.";
            return result;
        }
        std::copy(image.begin() + static_cast<std::ptrdiff_t>(cv + 4U),
                  image.begin() + static_cast<std::ptrdiff_t>(cv + 20U), result.guid.begin());
        result.age = read32(image, cv + 20U);
        if (result.age == 0U) {
            result.error = "CodeView PDB age must be greater than zero.";
            return result;
        }
        const std::size_t path_begin = cv + 24U;
        const std::size_t path_end = cv + result.codeview_size;
        const auto terminator = std::find(image.begin() + static_cast<std::ptrdiff_t>(path_begin),
            image.begin() + static_cast<std::ptrdiff_t>(path_end), 0U);
        if (terminator == image.begin() + static_cast<std::ptrdiff_t>(path_end)) {
            result.error = "CodeView PDB path is not NUL-terminated within SizeOfData.";
            return result;
        }
        result.pdb_path.assign(image.begin() + static_cast<std::ptrdiff_t>(path_begin), terminator);
        if (result.pdb_path.empty() || !std::all_of(result.pdb_path.begin(), result.pdb_path.end(),
            [](unsigned char c) { return c >= 0x20U && c <= 0x7eU; })) {
            result.error = "CodeView PDB path is empty or contains non-printable bytes.";
            return result;
        }
    }
    if (!found_codeview) {
        result.error = "PE Debug Directory has no CodeView entry.";
        return result;
    }
    result.valid = true;
    return result;
}

Pe64CertificateTableInfo inspect_pe64_certificate_table(
    const std::vector<std::uint8_t>& image) {
    Pe64CertificateTableInfo result;
    PeLayout layout;
    if (!read_layout(image, layout, result.error)) return result;
    const std::size_t entry = layout.optional_offset + kDataDirectoryOffset + kSecurityDirectoryIndex * 8U;
    result.file_offset = read32(image, entry);
    result.size = read32(image, entry + 4U);
    result.present = result.file_offset != 0U || result.size != 0U;
    if (!result.present) {
        result.valid = true;
        return result;
    }
    if (result.file_offset == 0U || result.size == 0U || (result.file_offset & 7U) != 0U ||
        result.file_offset > image.size() || result.size > image.size() - result.file_offset) {
        result.error = "PE Certificate Table file offset, alignment or size is invalid.";
        return result;
    }
    std::size_t cursor = result.file_offset;
    const std::size_t end = cursor + result.size;
    while (cursor < end) {
        if (!range_fits(end, cursor, 8U)) {
            result.error = "WIN_CERTIFICATE header is truncated.";
            return result;
        }
        const std::uint32_t length = read32(image, cursor);
        const std::uint16_t revision = read16(image, cursor + 4U);
        const std::uint16_t type = read16(image, cursor + 6U);
        if (length < 8U || length > end - cursor ||
            (revision != kWinCertRevision10 && revision != kWinCertRevision20) || type == 0U) {
            result.error = "WIN_CERTIFICATE length, revision or type is invalid.";
            return result;
        }
        std::uint64_t aligned_length64 = 0U;
        if (!align_up(length, 8U, aligned_length64) || aligned_length64 > end - cursor) {
            result.error = "WIN_CERTIFICATE aligned length exceeds the Certificate Table.";
            return result;
        }
        const std::size_t aligned_length = static_cast<std::size_t>(aligned_length64);
        for (std::size_t pad = cursor + length; pad < cursor + aligned_length; ++pad) {
            if (image[pad] != 0U) {
                result.error = "WIN_CERTIFICATE alignment padding is not zero-filled.";
                return result;
            }
        }
        if (type == kWinCertTypePkcsSignedData) {
            if (length <= 8U || image[cursor + 8U] != 0x30U) {
                result.error = "PKCS#7 SignedData payload is not a DER SEQUENCE.";
                return result;
            }
            ++result.pkcs7_entry_count;
        }
        ++result.entry_count;
        cursor += aligned_length;
    }
    if (cursor != end || result.entry_count == 0U) {
        result.error = "Certificate Table entries do not exactly cover the declared table size.";
        return result;
    }
    result.valid = true;
    return result;
}

} // namespace xfrontend
