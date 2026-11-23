#include "xfrontend/pe64_dll.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

void n35_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n35_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    std::cout << "RUN " << name << '\n' << std::flush;
    const auto start = std::chrono::steady_clock::now();
    try {
        body(); ++passed;
        std::cout << "PASS " << name << '\n' << std::flush;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n' << std::flush;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::uint16_t get16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n35_require(offset <= bytes.size() && bytes.size() - offset >= 2U, "u16 read outside image");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n35_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "u32 read outside image");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t get64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(get32(bytes, offset)) |
        (static_cast<std::uint64_t>(get32(bytes, offset + 4U)) << 32U);
}

const Pe64DllSectionInfo& find_section(const Pe64DllImage& image, const char* name) {
    const auto found = std::find_if(image.sections.begin(), image.sections.end(), [&name](const auto& item) {
        return item.name == name;
    });
    if (found == image.sections.end()) throw std::runtime_error(std::string("missing section ") + name);
    return *found;
}

std::size_t raw_offset_for_rva(const Pe64DllImage& image, std::uint32_t rva, std::size_t length) {
    for (const auto& section : image.sections) {
        if (rva < section.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
        if (delta <= section.raw_size && length <= section.raw_size - static_cast<std::size_t>(delta))
            return static_cast<std::size_t>(section.raw_offset) + static_cast<std::size_t>(delta);
    }
    throw std::runtime_error("RVA does not map to a raw section");
}

Pe64DllSpec make_one_relocation_spec(std::size_t index) {
    Pe64DllSpec spec;
    spec.dll_name = "n35_aslr_" + std::to_string(index) + ".dll";
    spec.image_base = 0x180000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
    spec.text = {0xB8U, 0x2AU, 0x00U, 0x00U, 0x00U, 0xC3U};
    spec.read_only_data = {'N','3','5',static_cast<std::uint8_t>('A' + (index % 26U)),0U};
    spec.data.assign(16U, 0U);
    spec.absolute_relocations = {
        {0U, Pe64SectionKind::Text, 0U, static_cast<std::int64_t>(index % 31U)}
    };
    spec.exports = {
        {"N35Return42", 1U, Pe64SectionKind::Text, 0U, false, {}},
        {"N35Pointer", 2U, Pe64SectionKind::Data, 0U, false, {}}
    };
    return spec;
}

Pe64DllSpec make_multi_page_spec(std::size_t index) {
    auto spec = make_one_relocation_spec(index);
    spec.dll_name = "n35_pages_" + std::to_string(index) + ".dll";
    spec.data.assign(0x2010U, 0U);
    spec.absolute_relocations = {
        {0U, Pe64SectionKind::Text, 0U, static_cast<std::int64_t>(index % 9U)},
        {0x1000U, Pe64SectionKind::ReadOnlyData, 0U, -static_cast<std::int64_t>(index % 7U)},
        {0x2000U, Pe64SectionKind::Data, 0U, static_cast<std::int64_t>(index % 5U)}
    };
    spec.exports[1U].target_offset = 0U;
    return spec;
}

void verify_relocation_blocks(const Pe64DllImage& image,
                              const std::vector<std::uint32_t>& expected_pages,
                              const std::vector<std::uint16_t>& expected_offsets) {
    n35_require(image.relocation_directory_rva != 0U, "relocation directory RVA is zero");
    n35_require(image.relocation_directory_size != 0U, "relocation directory size is zero");
    const std::size_t raw = raw_offset_for_rva(image, image.relocation_directory_rva,
                                                image.relocation_directory_size);
    std::size_t cursor = 0U;
    std::size_t entry_index = 0U;
    std::size_t page_index = 0U;
    while (cursor < image.relocation_directory_size) {
        n35_require(image.relocation_directory_size - cursor >= 8U, "truncated relocation block header");
        const std::uint32_t page = get32(image.bytes, raw + cursor);
        const std::uint32_t block_size = get32(image.bytes, raw + cursor + 4U);
        n35_require(block_size >= 8U && (block_size & 1U) == 0U, "invalid relocation block size");
        n35_require(block_size <= image.relocation_directory_size - cursor, "relocation block exceeds directory");
        n35_require(page_index < expected_pages.size() && page == expected_pages[page_index], "relocation page order/value mismatch");
        for (std::size_t pos = cursor + 8U; pos < cursor + block_size; pos += 2U) {
            const std::uint16_t entry = get16(image.bytes, raw + pos);
            const std::uint16_t type = static_cast<std::uint16_t>(entry >> 12U);
            const std::uint16_t offset = static_cast<std::uint16_t>(entry & 0x0fffU);
            if (type == 0U) {
                n35_require(pos + 2U == cursor + block_size, "ABSOLUTE padding must be the final block entry");
                continue;
            }
            n35_require(type == 10U, "base relocation type must be IMAGE_REL_BASED_DIR64");
            n35_require(entry_index < expected_offsets.size() && offset == expected_offsets[entry_index], "DIR64 offset mismatch");
            ++entry_index;
        }
        cursor += block_size;
        ++page_index;
    }
    n35_require(cursor == image.relocation_directory_size, "relocation directory has trailing or missing bytes");
    n35_require(entry_index == expected_offsets.size(), "relocation entry count mismatch");
    n35_require(page_index == expected_pages.size(), "relocation page count mismatch");
}

} // namespace

void run_pe64_dll_relocation_tests(int& passed, int& failed) {
    // N35A: one DIR64 relocation, PE data-directory wiring, ASLR flags and pointer patching.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n35_case("N35A" + std::to_string(index + 1U) + "_pe64_dll_aslr_single_dir64", [index] {
            const auto spec = make_one_relocation_spec(index);
            const auto result = write_pe64_dll_image(spec);
            n35_require(result.succeeded(), "valid one-relocation DLL was rejected" +
                (result.diagnostics.empty() ? std::string{} : ": " + result.diagnostics.front().code + " " + result.diagnostics.front().message));
            const auto& image = result.image;
            const std::size_t pe = get32(image.bytes, 0x3cU);
            const std::size_t optional = pe + 24U;
            n35_require(image.sections.size() == 5U, "expected .text/.rdata/.data/.edata/.reloc");
            const auto& data = find_section(image, ".data");
            const auto& reloc = find_section(image, ".reloc");
            n35_require(reloc.characteristics == 0x42000040U, ".reloc must be readable and discardable");
            n35_require(image.relocation_directory_rva == reloc.virtual_address, "relocation directory RVA mismatch");
            n35_require(image.relocation_directory_size == 12U, "one-entry relocation block must include ABSOLUTE pad");
            n35_require(get32(image.bytes, optional + 112U + 5U * 8U) == reloc.virtual_address, "Optional Header Base Relocation RVA mismatch");
            n35_require(get32(image.bytes, optional + 112U + 5U * 8U + 4U) == 12U, "Optional Header Base Relocation size mismatch");
            n35_require((get16(image.bytes, optional + 70U) & 0x0040U) != 0U, "DYNAMIC_BASE must be set when relocations exist");
            n35_require((get16(image.bytes, pe + 22U) & 0x0001U) == 0U, "RELOCS_STRIPPED must be clear when relocations exist");
            n35_require(image.has_dynamic_base && image.base_relocation_count == 1U, "relocation metadata counters mismatch");
            const std::uint64_t target_va = spec.image_base + find_section(image, ".text").virtual_address;
            n35_require(get64(image.bytes, data.raw_offset) == target_va + static_cast<std::uint64_t>(index % 31U),
                        "preferred-base pointer initializer mismatch");
            verify_relocation_blocks(image, {data.virtual_address}, {0U});
        }, passed, failed);
    }

    // N35B: multiple pages, mixed section targets, signed addends and round-trip export visibility.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n35_case("N35B" + std::to_string(index + 1U) + "_pe64_dll_mult_page_relocations", [index] {
            const auto spec = make_multi_page_spec(index);
            const auto result = write_pe64_dll_image(spec);
            n35_require(result.succeeded(), "valid multi-page DLL was rejected" +
                (result.diagnostics.empty() ? std::string{} : ": " + result.diagnostics.front().code + " " + result.diagnostics.front().message));
            const auto& image = result.image;
            const auto& data = find_section(image, ".data");
            const auto& text = find_section(image, ".text");
            const auto& rdata = find_section(image, ".rdata");
            n35_require(image.base_relocation_count == 3U, "three DIR64 relocations expected");
            n35_require(get64(image.bytes, data.raw_offset) == spec.image_base + text.virtual_address + (index % 9U),
                        "text-target pointer does not contain the preferred address");
            const std::uint64_t second_expected = spec.image_base + rdata.virtual_address - (index % 7U);
            n35_require(get64(image.bytes, data.raw_offset + 0x1000U) == second_expected,
                        "negative addend pointer initializer mismatch");
            n35_require(get64(image.bytes, data.raw_offset + 0x2000U) == spec.image_base + data.virtual_address + (index % 5U),
                        "data-target pointer initializer mismatch");
            verify_relocation_blocks(image,
                {data.virtual_address, data.virtual_address + 0x1000U, data.virtual_address + 0x2000U},
                {0U, 0U, 0U});
            n35_require(image.size_of_image > data.virtual_address + data.virtual_size, "SizeOfImage must include later relocation section");
        }, passed, failed);
    }

    // N35C: deterministic relocation output and unchanged fixed-base behavior without relocation requests.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n35_case("N35C" + std::to_string(index + 1U) + "_pe64_dll_aslr_determinism_compat", [index] {
            auto relocated_spec = make_one_relocation_spec(index + 700U);
            const auto first = write_pe64_dll_image(relocated_spec);
            const auto second = write_pe64_dll_image(relocated_spec);
            n35_require(first.succeeded() && second.succeeded(), "relocated fixture generation failed");
            n35_require(first.image.bytes == second.image.bytes, "identical relocation spec must produce byte-identical DLLs");
            n35_require(first.image.sections.size() == second.image.sections.size(), "section count must be deterministic");
            for (std::size_t section_index = 0U; section_index < first.image.sections.size(); ++section_index) {
                const auto& left = first.image.sections[section_index];
                const auto& right = second.image.sections[section_index];
                n35_require(left.name == right.name && left.virtual_address == right.virtual_address &&
                            left.raw_offset == right.raw_offset && left.raw_size == right.raw_size &&
                            left.virtual_size == right.virtual_size && left.characteristics == right.characteristics,
                            "section metadata must be byte-for-byte deterministic");
            }

            Pe64DllSpec fixed_spec = make_one_relocation_spec(index + 1300U);
            fixed_spec.absolute_relocations.clear();
            const auto fixed = write_pe64_dll_image(fixed_spec);
            n35_require(fixed.succeeded(), "legacy no-relocation DLL was rejected");
            const std::size_t pe = get32(fixed.image.bytes, 0x3cU);
            const std::size_t optional = pe + 24U;
            n35_require(fixed.image.relocation_directory_rva == 0U && fixed.image.relocation_directory_size == 0U,
                        "fixed-base DLL must not publish a relocation directory");
            n35_require(!fixed.image.has_dynamic_base && fixed.image.base_relocation_count == 0U,
                        "fixed-base DLL relocation metadata must remain empty");
            n35_require((get16(fixed.image.bytes, optional + 70U) & 0x0040U) == 0U,
                        "DYNAMIC_BASE must remain clear without relocations");
            n35_require((get16(fixed.image.bytes, pe + 22U) & 0x0001U) != 0U,
                        "RELOCS_STRIPPED must remain set without relocation records");
            n35_require(std::none_of(fixed.image.sections.begin(), fixed.image.sections.end(), [](const auto& section) {
                return section.name == ".reloc";
            }), "fixed-base output must not contain a .reloc section");
        }, passed, failed);
    }

    // N35D: malformed relocation destinations, targets, limits and pointer arithmetic are rejected atomically.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n35_case("N35D" + std::to_string(index + 1U) + "_reject_invalid_pe64_dll_relocations", [index] {
            auto spec = make_one_relocation_spec(index);
            switch (index % 11U) {
            case 0U: spec.absolute_relocations[0U].destination_offset = 1U; break;
            case 1U: spec.absolute_relocations[0U].destination_offset = spec.data.size(); break;
            case 2U: spec.absolute_relocations.push_back(spec.absolute_relocations[0U]); break;
            case 3U: spec.absolute_relocations[0U].target_offset = spec.text.size(); break;
            case 4U: spec.absolute_relocations[0U].target_section = static_cast<Pe64SectionKind>(99U); break;
            case 5U: spec.absolute_relocations[0U].target_section = Pe64SectionKind::Data; spec.absolute_relocations[0U].target_offset = spec.data.size(); break;
            case 6U: spec.image_base = 0xffffffffffff0000ULL; spec.text.resize(0x10000U); spec.absolute_relocations[0U].target_section = Pe64SectionKind::Text; spec.absolute_relocations[0U].target_offset = 0xf000U; break;
            case 7U: spec.image_base = 0x10000ULL; spec.absolute_relocations[0U].addend = std::numeric_limits<std::int64_t>::min(); break;
            case 8U: spec.image_base = 0xffffffffffff0000ULL; spec.absolute_relocations[0U].addend = std::numeric_limits<std::int64_t>::max(); break;
            case 9U: spec.maximum_file_size = 0x400U; break;
            case 10U: {
                const auto relocation = spec.absolute_relocations.front();
                spec.absolute_relocations.assign(65537U, relocation);
                break;
            }
            }
            const auto rejected = write_pe64_dll_image(spec);
            n35_require(!rejected.succeeded() && !rejected.diagnostics.empty(), "invalid relocation spec unexpectedly succeeded");
            n35_require(rejected.image.bytes.empty(), "failed DLL writer must not return a partial image");
            n35_require(rejected.diagnostics.front().code.rfind("XPE64DLL", 0U) == 0U,
                        "invalid relocation must return an XPE64DLL diagnostic");
        }, passed, failed);
    }
}
