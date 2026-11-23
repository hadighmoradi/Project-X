#include "xfrontend/pe64_dll.hpp"
#include "xfrontend/pe64_exports.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

void n34_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n34_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body(); ++passed;
        std::cout << "PASS " << name << '\n';
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::uint16_t get16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n34_require(offset + 2U <= bytes.size(), "u16 read outside output image");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n34_require(offset + 4U <= bytes.size(), "u32 read outside output image");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

Pe64DllSpec make_spec(std::size_t index) {
    Pe64DllSpec spec;
    spec.dll_name = "n34_fixture_" + std::to_string(index) + ".dll";
    spec.image_base = 0x180000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
    spec.text = {0xB8U, 0x2AU, 0x00U, 0x00U, 0x00U, 0xC3U,
                 0xB8U, static_cast<std::uint8_t>(index & 0xffU), 0U, 0U, 0U, 0xC3U};
    spec.read_only_data = {'N','3','4',static_cast<std::uint8_t>('A' + (index % 26U)),0U};
    spec.data = {static_cast<std::uint8_t>(index & 0xffU), 0x22U, 0x33U, 0x44U};
    const std::uint32_t base = static_cast<std::uint32_t>(100U + (index % 20000U));
    spec.exports = {
        {"N34Return42", base, Pe64SectionKind::Text, 0U, false, {}},
        {"N34Alias42", base, Pe64SectionKind::Text, 0U, false, {}},
        {"N34DataValue", base + 1U, Pe64SectionKind::Data, 0U, false, {}},
        {"", base + 2U, Pe64SectionKind::Text, 6U, false, {}},
        {"N34Forwarded", base + 3U, Pe64SectionKind::Text, 0U, true, "KERNELBASE.Sleep"},
        {"N34ReadOnlyData", base + 4U, Pe64SectionKind::ReadOnlyData, 0U, false, {}},
        {"N34SparseOrdinal", base + 6U, Pe64SectionKind::Text, 6U, false, {}}
    };
    return spec;
}


} // namespace

void run_pe64_dll_tests(int& passed, int& failed) {
    // N34A: image headers, sections, alignment, characteristics and stable RVA layout.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n34_case("N34A" + std::to_string(index + 1U) + "_pe64_dll_headers_sections", [index] {
            const auto spec = make_spec(index);
            const auto result = write_pe64_dll_image(spec);
            n34_require(result.succeeded(), "valid DLL spec was rejected" +
                (result.diagnostics.empty() ? std::string{} : ": " + result.diagnostics.front().code + " " + result.diagnostics.front().message));
            const auto& bytes = result.image.bytes;
            n34_require(bytes[0] == 'M' && bytes[1] == 'Z', "DOS signature mismatch");
            const std::size_t pe = get32(bytes, 0x3cU);
            n34_require(pe + 24U + 240U <= bytes.size(), "PE header truncated");
            n34_require(bytes[pe] == 'P' && bytes[pe + 1U] == 'E', "PE signature mismatch");
            n34_require(get16(bytes, pe + 4U) == 0x8664U, "AMD64 machine mismatch");
            n34_require((get16(bytes, pe + 22U) & 0x2000U) != 0U, "IMAGE_FILE_DLL flag missing");
            const std::size_t optional = pe + 24U;
            n34_require(get16(bytes, optional) == 0x020bU, "PE32+ optional header expected");
            n34_require(get32(bytes, optional + 16U) == 0U, "CRT-free DLL must have no entry point");
            n34_require(get32(bytes, optional + 56U) == result.image.size_of_image, "SizeOfImage mismatch");
            n34_require(get32(bytes, optional + 60U) == result.image.size_of_headers, "SizeOfHeaders mismatch");
            n34_require((get16(bytes, optional + 70U) & 0x0100U) != 0U, "NX compatibility flag missing");
            n34_require(result.image.sections.size() == 4U, "expected text/rdata/data/edata sections");
            for (const auto& section : result.image.sections) {
                n34_require((section.virtual_address & 0xfffU) == 0U, "section RVA is not 4-KiB aligned");
                n34_require((section.raw_offset & 0x1ffU) == 0U, "section raw pointer is not file-aligned");
                n34_require((section.raw_size & 0x1ffU) == 0U, "section raw size is not file-aligned");
            }
            const auto text = std::find_if(result.image.sections.begin(), result.image.sections.end(), [](const auto& s) { return s.name == ".text"; });
            const auto edata = std::find_if(result.image.sections.begin(), result.image.sections.end(), [](const auto& s) { return s.name == ".edata"; });
            n34_require(text != result.image.sections.end() && text->characteristics == 0x60000020U, "text section flags mismatch");
            n34_require(edata != result.image.sections.end() && edata->characteristics == 0x40000040U, "edata must be read-only");
            n34_require(result.image.export_directory_rva == edata->virtual_address, "export directory must start in .edata");
            n34_require(result.image.bytes.size() >= result.image.size_of_headers, "headers exceed file size");
            n34_require(result.image.bytes.size() < spec.maximum_file_size, "unexpected maximum-file-size violation");
        }, passed, failed);
    }

    // N34B: name table, aliases, ordinal-only slots, data targets and forwarders.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n34_case("N34B" + std::to_string(index + 1U) + "_pe64_dll_export_directory", [index] {
            const auto spec = make_spec(index);
            const auto written = write_pe64_dll_image(spec);
            n34_require(written.succeeded(), "DLL image generation failed");
            const auto parsed = read_pe64_export_directory(written.image.bytes);
            n34_require(parsed.succeeded(), "N33 reader rejected N34 output" +
                (parsed.diagnostics.empty() ? std::string{} : ": " + parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message));
            n34_require(parsed.directory.dll_name == spec.dll_name, "DLL name mismatch");
            n34_require(parsed.directory.image_base == spec.image_base, "image base mismatch");
            n34_require(parsed.directory.exports.size() == 7U, "export binding count mismatch");
            const auto& exports = parsed.directory.exports;
            const std::uint32_t base = spec.exports.front().ordinal;
            const auto named = [&exports](const std::string& name) -> const Pe64ExportSymbol& {
                const auto found = std::find_if(exports.begin(), exports.end(), [&name](const auto& item) { return item.name == name; });
                if (found == exports.end()) throw std::runtime_error("named export not found: " + name);
                return *found;
            };
            n34_require(named("N34Return42").ordinal == base, "function ordinal mismatch");
            n34_require(named("N34Alias42").ordinal == base, "alias ordinal must match original");
            n34_require(named("N34DataValue").ordinal == base + 1U, "data export ordinal mismatch");
            const auto rodata_section = std::find_if(written.image.sections.begin(), written.image.sections.end(), [](const auto& s) { return s.name == ".rdata"; });
            n34_require(rodata_section != written.image.sections.end(), "read-only data section missing");
            n34_require(named("N34ReadOnlyData").ordinal == base + 4U &&
                        named("N34ReadOnlyData").function_rva == rodata_section->virtual_address,
                        "read-only data export RVA mismatch");
            n34_require(named("N34Forwarded").forwarded && named("N34Forwarded").forwarder == "KERNELBASE.Sleep", "forwarder mapping mismatch");
            const auto ordinal_only = std::find_if(exports.begin(), exports.end(), [](const auto& item) { return item.name.empty(); });
            n34_require(ordinal_only != exports.end() && ordinal_only->ordinal == base + 2U, "ordinal-only export mismatch");
            n34_require(named("N34SparseOrdinal").ordinal == base + 6U,
                        "sparse ordinal export mapping mismatch");
            n34_require(parsed.directory.ordinal_base == base,
                        "sparse export ordinal base mismatch");
            n34_require(written.image.export_name_count == 6U && written.image.export_ordinal_count == 6U,
                        "name/ordinal counters mismatch");
            const auto import_lib = write_coff_import_library_from_pe64_dll(written.image.bytes,
                {{"N34DataValue", 0U, CoffImportType::Data}});
            n34_require(import_lib.succeeded() && import_lib.import_count == 7U,
                        "N34 DLL output should round-trip through N33 import-library reader");
        }, passed, failed);
    }

    // N34C: deterministic image bytes and direct import-library round trip.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n34_case("N34C" + std::to_string(index + 1U) + "_pe64_dll_determinism_roundtrip", [index] {
            const auto spec = make_spec(index + 500U);
            const auto first = write_pe64_dll_image(spec);
            const auto second = write_pe64_dll_image(spec);
            n34_require(first.succeeded() && second.succeeded(), "deterministic fixture generation failed");
            n34_require(first.image.bytes == second.image.bytes, "same DLL specification must be byte-identical");
            n34_require(first.image.sections.size() == second.image.sections.size(), "section count differs");
            const auto parsed = read_pe64_export_directory(first.image.bytes);
            n34_require(parsed.succeeded(), "round-trip export parse failed");
            const auto lib1 = write_coff_import_library_from_pe64_dll(first.image.bytes,
                {{"N34DataValue", 0U, CoffImportType::Data}});
            const auto lib2 = write_coff_import_library_from_pe64_dll(second.image.bytes,
                {{"N34DataValue", 0U, CoffImportType::Data}});
            n34_require(lib1.succeeded() && lib2.succeeded() && lib1.bytes == lib2.bytes,
                        "same DLL must yield byte-identical import libraries");
            n34_require(lib1.import_count == 7U, "alias and ordinal import record count mismatch");
        }, passed, failed);
    }

    // N34D: malformed specs and boundary violations are rejected with stable diagnostics.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n34_case("N34D" + std::to_string(index + 1U) + "_reject_invalid_pe64_dll_spec", [index] {
            auto spec = make_spec(index);
            switch (index % 18U) {
            case 0U: spec.dll_name.clear(); break;
            case 1U: spec.text.clear(); break;
            case 2U: spec.exports.clear(); break;
            case 3U: spec.file_alignment = 3U; break;
            case 4U: spec.section_alignment = 0x200U; spec.file_alignment = 0x400U; break;
            case 5U: spec.image_base += 1U; break;
            case 6U: spec.exports[0U].ordinal = 0U; break;
            case 7U: spec.exports[0U].ordinal = 65536U; break;
            case 8U: spec.exports[1U].name = spec.exports[0U].name; break;
            case 9U: spec.exports[0U].target_offset = spec.text.size(); break;
            case 10U: spec.exports[3U].forwarded = true; spec.exports[3U].forwarder = ".Sleep"; break;
            case 11U: spec.exports[0U].name = std::string("N34") + static_cast<char>(0xc3) + static_cast<char>(0xa9); break;
            case 12U: spec.exports[0U].target_section = static_cast<Pe64SectionKind>(99U); break;
            case 13U: spec.maximum_file_size = 512U; break;
            case 14U: spec.exports[1U].target_offset = 1U; break;
            case 15U: spec.exports[1U].ordinal = spec.exports[0U].ordinal + 4096U; break;
            case 16U: spec.section_alignment = 0x1000U; spec.file_alignment = 0x10000U; break;
            case 17U: spec.image_base = 0U; break;
            }
            const auto rejected = write_pe64_dll_image(spec);
            n34_require(!rejected.succeeded() && !rejected.diagnostics.empty(), "invalid DLL spec unexpectedly succeeded");
            n34_require(rejected.image.bytes.empty(), "failed writer must not return a partial PE image");
            const std::string code = rejected.diagnostics.front().code;
            n34_require(code.rfind("XPE64DLL", 0U) == 0U, "invalid input must produce an N34 diagnostic code");
        }, passed, failed);
    }
}
