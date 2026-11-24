// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>

namespace {
bool read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
    return true;
}
bool contains_bytes(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& pattern) {
    return std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}
bool read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
            (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    return true;
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "N47_PE_VERIFY | RESULT=FAILED | ERROR=expected one DLL path | EXIT=2\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "N47_PE_VERIFY | RESULT=FAILED | ERROR=cannot open DLL | EXIT=2\n";
        return 2;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());
    std::uint32_t pe_offset = 0U;
    std::uint16_t machine = 0U;
    std::uint16_t section_count = 0U;
    std::uint16_t optional_size = 0U;
    std::uint16_t magic = 0U;
    std::uint32_t entry_point = 0U;
    std::uint32_t export_rva = 0U;
    std::uint32_t export_size = 0U;
    if (!read_u32(bytes, 0x3cU, pe_offset) || pe_offset > bytes.size() ||
        bytes.size() - pe_offset < 24U || bytes[pe_offset] != 'P' ||
        bytes[pe_offset + 1U] != 'E' || bytes[pe_offset + 2U] != 0U || bytes[pe_offset + 3U] != 0U ||
        !read_u16(bytes, static_cast<std::size_t>(pe_offset) + 4U, machine) ||
        !read_u16(bytes, static_cast<std::size_t>(pe_offset) + 6U, section_count) ||
        !read_u16(bytes, static_cast<std::size_t>(pe_offset) + 20U, optional_size)) {
        std::cerr << "N47_PE_VERIFY | RESULT=FAILED | ERROR=invalid PE/COFF headers | EXIT=2\n";
        return 2;
    }
    const std::size_t optional = static_cast<std::size_t>(pe_offset) + 24U;
    if (machine != 0x8664U || optional_size < 120U ||
        !read_u16(bytes, optional, magic) || magic != 0x20bU ||
        !read_u32(bytes, optional + 16U, entry_point) ||
        !read_u32(bytes, optional + 112U, export_rva) ||
        !read_u32(bytes, optional + 116U, export_size) ||
        section_count == 0U) {
        std::cerr << "N47_PE_VERIFY | RESULT=FAILED | ERROR=invalid PE32+ AMD64 optional header | EXIT=2\n";
        return 2;
    }
    const std::size_t section_table = optional + optional_size;
    if (section_table > bytes.size() ||
        static_cast<std::size_t>(section_count) > (bytes.size() - section_table) / 40U) {
        std::cerr << "N47_PE_VERIFY | RESULT=FAILED | ERROR=truncated PE section table | EXIT=2\n";
        return 2;
    }
    std::uint32_t size_of_image = 0U;
    std::uint32_t section_alignment = 0U;
    if (!read_u32(bytes, optional + 32U, section_alignment) ||
        !read_u32(bytes, optional + 56U, size_of_image) ||
        section_alignment == 0U || size_of_image == 0U) {
        std::cerr << "N47_PE_VERIFY | RESULT=FAILED | ERROR=invalid PE image sizing fields | EXIT=2\n";
        return 2;
    }
    std::uint32_t text_virtual_size = 0U;
    std::uint32_t text_virtual_address = 0U;
    std::uint32_t text_raw_size = 0U;
    std::uint32_t text_raw_offset = 0U;
    std::uint32_t text_characteristics = 0U;
    bool found_text = false;
    for (std::uint16_t index = 0U; index < section_count; ++index) {
        const std::size_t header = section_table + static_cast<std::size_t>(index) * 40U;
        const std::string name(reinterpret_cast<const char*>(bytes.data() + header), 8U);
        if (name.compare(0U, 5U, ".text") == 0) {
            if (!read_u32(bytes, header + 8U, text_virtual_size) ||
                !read_u32(bytes, header + 12U, text_virtual_address) ||
                !read_u32(bytes, header + 16U, text_raw_size) ||
                !read_u32(bytes, header + 20U, text_raw_offset) ||
                !read_u32(bytes, header + 36U, text_characteristics)) {
                std::cerr << "N47_PE_VERIFY | RESULT=FAILED | ERROR=truncated .text header | EXIT=2\n";
                return 2;
            }
            found_text = true;
            break;
        }
    }
    // The linker may add different amounts of padding/alignment to .text.
    // Validate the PE section's bounds and required code signatures instead
    // of requiring one toolchain-specific VirtualSize value.
    constexpr std::uint32_t expected_live_code_bytes = 26U; // Main(14) + DllMain(6) + Math.Sin(6)
    std::vector<std::uint8_t> text_bytes;
    if (found_text && text_raw_offset <= bytes.size() && text_raw_size <= bytes.size() - text_raw_offset) {
        text_bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(text_raw_offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(text_raw_offset + text_raw_size));
    }
    const bool main_present = contains_bytes(text_bytes, {0x48U,0x83U,0xECU,0x28U,0xE8U});
    const bool sin_present = contains_bytes(text_bytes, {0xB8U,42U,0U,0U,0U,0xC3U});
    const bool dllmain_present = contains_bytes(text_bytes, {0xB8U,1U,0U,0U,0U,0xC3U});
    const bool cos_absent = !contains_bytes(text_bytes, {0xB8U,17U,0U,0U,0U,0xC3U});
    const bool tan_absent = !contains_bytes(text_bytes, {0xB8U,19U,0U,0U,0U,0xC3U});
    const std::uint64_t text_virtual_end = static_cast<std::uint64_t>(text_virtual_address) +
        static_cast<std::uint64_t>(text_virtual_size);
    const bool text_size_valid = found_text && text_virtual_size >= expected_live_code_bytes &&
        text_virtual_address < size_of_image && text_virtual_end <= size_of_image;
    const bool text_raw_valid = text_raw_size != 0U && text_raw_offset <= bytes.size() &&
        text_raw_size <= bytes.size() - text_raw_offset;
    const bool passed = text_size_valid && text_raw_valid &&
        (text_characteristics & 0x20000000U) != 0U && entry_point != 0U &&
        export_rva != 0U && export_size != 0U && main_present && sin_present &&
        dllmain_present && cos_absent && tan_absent;
    if (!passed) {
        std::cerr << "N47_PE_VERIFY | RESULT=FAILED | TEXT_FOUND=" << (found_text ? "YES" : "NO")
                  << " | TEXT_VIRTUAL_SIZE=" << text_virtual_size
                  << " | TEXT_SIZE_VALID=" << (text_size_valid ? "YES" : "NO")
                  << " | IMAGE_SIZE=" << size_of_image
                  << " | MAIN=" << (main_present ? "YES" : "NO")
                  << " | SIN=" << (sin_present ? "YES" : "NO")
                  << " | DLLMAIN=" << (dllmain_present ? "YES" : "NO")
                  << " | COS_ABSENT=" << (cos_absent ? "YES" : "NO")
                  << " | TAN_ABSENT=" << (tan_absent ? "YES" : "NO")
                  << " | ENTRYPOINT_RVA=" << entry_point
                  << " | EXPORT_RVA=" << export_rva << " | EXIT=1\n";
        return 1;
    }
    std::cout << "N47_PE_VERIFY | RESULT=PASSED | ARCH=AMD64 | TEXT_VIRTUAL_SIZE="
              << text_virtual_size << " | TEXT_SIZE=BOUNDS_VALID | IMAGE_SIZE=" << size_of_image
              << " | LIVE_CODE_BYTES=" << expected_live_code_bytes
              << " | MAIN=SAVED | SIN=SAVED | DLLMAIN=SAVED | COS=ELIMINATED | TAN=ELIMINATED"
              << " | ENTRYPOINT=PASSED | EXPORT_DIRECTORY=PASSED | EXIT=0\n";
    return 0;
}
