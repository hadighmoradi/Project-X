#include "xfrontend/pe64_debug_signing.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void test_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(id)) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(id, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(id, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
}

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4U, "32-bit read outside PE image");
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

Pe64DllImage make_base_image() {
    Pe64DllSpec spec;
    spec.dll_name = "n44_debug_test.dll";
    spec.text = {0xc3U, 0x90U, 0x90U, 0x90U};
    spec.exports.push_back(Pe64DllExport{"N44Entry", 1U, Pe64SectionKind::Text, 0U, false, {}});
    const auto result = write_pe64_dll_image(spec);
    require(result.succeeded(), "base PE64 fixture could not be created");
    return result.image;
}

Pe64CodeViewSpec make_codeview_spec(std::size_t index) {
    Pe64CodeViewSpec spec;
    for (std::size_t i = 0U; i < spec.guid.size(); ++i)
        spec.guid[i] = static_cast<std::uint8_t>((index * 17U + i * 29U + 3U) & 0xffU);
    spec.age = static_cast<std::uint32_t>((index % 0x7ffffffeU) + 1U);
    spec.timestamp = static_cast<std::uint32_t>(0x65000000U + index);
    spec.pdb_path = "symbols/n44_" + std::to_string(index) + ".pdb";
    return spec;
}

std::size_t optional_header_offset(const std::vector<std::uint8_t>& image) {
    return static_cast<std::size_t>(read32(image, 0x3cU)) + 24U;
}

std::vector<std::uint8_t> make_certificate_image(std::size_t count, std::uint16_t type = 2U) {
    auto image = make_base_image().bytes;
    if (count == 0U) return image;
    const std::size_t offset = (image.size() + 7U) & ~static_cast<std::size_t>(7U);
    image.resize(offset, 0U);
    for (std::size_t i = 0U; i < count; ++i) {
        const std::size_t entry = image.size();
        image.resize(entry + 16U, 0U);
        write32(image, entry, 16U);
        write16(image, entry + 4U, 0x0200U);
        write16(image, entry + 6U, type);
        if (type == 2U) {
            image[entry + 8U] = 0x30U;
            image[entry + 9U] = 0x03U;
            image[entry + 10U] = 0x02U;
            image[entry + 11U] = 0x01U;
            image[entry + 12U] = static_cast<std::uint8_t>(i & 0xffU);
        } else {
            image[entry + 8U] = 0x41U;
        }
    }
    const std::size_t directory = optional_header_offset(image) + 112U + 4U * 8U;
    write32(image, directory, static_cast<std::uint32_t>(offset));
    write32(image, directory + 4U, static_cast<std::uint32_t>(count * 16U));
    return image;
}

void n44_codeview_roundtrip(std::size_t index) {
    const auto base = make_base_image();
    const auto spec = make_codeview_spec(index);
    const auto output = write_pe64_codeview_debug_directory(base, spec);
    require(output.succeeded(), "CodeView Debug Directory generation failed");
    require(output.image.sections.size() == base.sections.size() + 1U, ".debug section was not added");
    const auto inspected = inspect_pe64_codeview_debug_directory(output.image.bytes);
    require(inspected.valid && inspected.present, "generated CodeView record was not accepted by the inspector");
    require(inspected.guid == spec.guid && inspected.age == spec.age &&
            inspected.timestamp == spec.timestamp && inspected.pdb_path == spec.pdb_path,
            "RSDS GUID, age, timestamp or PDB path did not round-trip");
    require(output.debug.directory_size == 28U && output.debug.codeview_size == 25U + spec.pdb_path.size(),
            "IMAGE_DEBUG_DIRECTORY or CodeView data size is inconsistent");
}

void n44_invalid_debug_inputs(std::size_t index) {
    auto base = make_base_image();
    auto spec = make_codeview_spec(index);
    switch (index % 8U) {
    case 0U: spec.age = 0U; break;
    case 1U: spec.pdb_path.clear(); break;
    case 2U: spec.pdb_path = std::string("bad\0path.pdb", 12U); break;
    case 3U: spec.pdb_path.assign(1025U, 'p'); break;
    case 4U: spec.pdb_path = "bad\npath.pdb"; break;
    case 5U: base.bytes.clear(); break;
    case 6U: base.bytes[0] = 0U; break;
    default: {
        const auto already = write_pe64_codeview_debug_directory(base, spec);
        require(already.succeeded(), "initial CodeView setup failed");
        base = already.image;
        break;
    }
    }
    const auto output = write_pe64_codeview_debug_directory(base, spec);
    require(!output.succeeded() && !output.diagnostics.empty(), "invalid CodeView request was not rejected");
}

void n44_codeview_corruption_detection(std::size_t index) {
    const auto built = write_pe64_codeview_debug_directory(make_base_image(), make_codeview_spec(index));
    require(built.succeeded(), "CodeView fixture generation failed");
    auto bytes = built.image.bytes;
    const std::size_t optional = optional_header_offset(bytes);
    const std::size_t debug_entry = optional + 112U + 6U * 8U;
    const std::uint32_t debug_rva = read32(bytes, debug_entry);
    const auto& debug_section = built.image.sections.back();
    const std::size_t directory_offset = debug_section.raw_offset;
    switch (index % 6U) {
    case 0U: write32(bytes, debug_entry, 0U); break;
    case 1U: write32(bytes, debug_entry + 4U, 27U); break;
    case 2U: write32(bytes, directory_offset + 20U, debug_rva + 4U); break;
    case 3U: write32(bytes, directory_offset + 24U, 0xffffffffU); break;
    case 4U: bytes[debug_section.raw_offset + 28U] = 'N'; break;
    default: write32(bytes, directory_offset + 28U + 20U, 0U); break;
    }
    const auto inspected = inspect_pe64_codeview_debug_directory(bytes);
    require(!inspected.valid && !inspected.error.empty(), "corrupted CodeView metadata was not detected");
}

void n44_certificate_table_valid(std::size_t index) {
    const std::size_t count = index % 3U;
    const std::uint16_t type = (index % 4U == 0U) ? 0x0003U : 0x0002U;
    const auto bytes = make_certificate_image(count, type);
    const auto inspected = inspect_pe64_certificate_table(bytes);
    require(inspected.valid, "valid/absent certificate table was rejected");
    require(inspected.present == (count != 0U), "certificate table presence mismatch");
    const std::size_t expected_pkcs7 = type == 0x0002U ? count : 0U;
    require(inspected.entry_count == count && inspected.pkcs7_entry_count == expected_pkcs7,
            "certificate table entry counts mismatch");
}

void n44_certificate_table_corruption(std::size_t index) {
    auto bytes = make_certificate_image(1U);
    const std::size_t optional = optional_header_offset(bytes);
    const std::size_t directory = optional + 112U + 4U * 8U;
    const std::uint32_t start = read32(bytes, directory);
    switch (index % 8U) {
    case 0U: write32(bytes, directory, start + 1U); break;
    case 1U: write32(bytes, directory + 4U, 15U); break;
    case 2U: write32(bytes, directory, 0xfffffff8U); break;
    case 3U: write32(bytes, start, 7U); break;
    case 4U: write32(bytes, start, 0xffffffffU); break;
    case 5U: write16(bytes, start + 4U, 0x0300U); break;
    case 6U: bytes[start + 8U] = 0x31U; break;
    default: write32(bytes, start, 15U); bytes[start + 15U] = 0x7fU; break;
    }
    const auto inspected = inspect_pe64_certificate_table(bytes);
    require(!inspected.valid && !inspected.error.empty(), "corrupted WIN_CERTIFICATE table was not rejected");
}

} // namespace

void run_pe64_n44_tests(int& passed, int& failed) {
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N44A" + std::to_string(i + 1U) + "_codeview_rsds_debug_directory_roundtrip", [i] { n44_codeview_roundtrip(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N44B" + std::to_string(i + 1U) + "_invalid_codeview_debug_metadata_rejection", [i] { n44_invalid_debug_inputs(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N44C" + std::to_string(i + 1U) + "_codeview_corruption_and_pointer_validation", [i] { n44_codeview_corruption_detection(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N44D" + std::to_string(i + 1U) + "_authenticode_certificate_table_validation", [i] { n44_certificate_table_valid(i); n44_certificate_table_corruption(i); }, passed, failed);
}
