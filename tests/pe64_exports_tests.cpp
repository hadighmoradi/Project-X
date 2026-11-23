#include "xfrontend/pe64_exports.hpp"
#include "xfrontend/archive.hpp"
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

void n33_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n33_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
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

void put16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
    bytes.at(offset + 1U) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
}

void put64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
}

void put_string(std::vector<std::uint8_t>& bytes, std::size_t offset, const std::string& value) {
    n33_require(offset + value.size() + 1U <= bytes.size(), "fixture string overflow");
    std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    bytes[offset + value.size()] = 0U;
}

std::vector<std::uint8_t> make_pe64_dll_fixture(std::uint32_t ordinal_base = 17U,
                                                const std::string& dll_name = "n33_sample.dll") {
    std::vector<std::uint8_t> bytes(0x600U, 0U);
    bytes[0U] = 'M'; bytes[1U] = 'Z';
    put32(bytes, 0x3cU, 0x80U);
    bytes[0x80U] = 'P'; bytes[0x81U] = 'E';
    put16(bytes, 0x84U, 0x8664U);
    put16(bytes, 0x86U, 2U);
    put16(bytes, 0x94U, 240U);
    put16(bytes, 0x96U, 0x2022U); // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | DLL.

    const std::size_t optional = 0x98U;
    put16(bytes, optional, 0x020bU);
    put64(bytes, optional + 24U, 0x180000000ULL);
    put32(bytes, optional + 32U, 0x1000U);
    put32(bytes, optional + 36U, 0x200U);
    put32(bytes, optional + 56U, 0x3000U);
    put32(bytes, optional + 60U, 0x200U);
    put32(bytes, optional + 108U, 16U);
    put32(bytes, optional + 112U, 0x2000U);
    put32(bytes, optional + 116U, 0x200U);

    const std::size_t section_table = optional + 240U;
    const auto write_section = [&bytes](std::size_t index, const char* name,
            std::uint32_t virtual_size, std::uint32_t virtual_address,
            std::uint32_t raw_size, std::uint32_t raw_offset, std::uint32_t characteristics) {
        const std::size_t offset = section_table + index * 40U;
        for (std::size_t i = 0U; i < 8U && name[i] != '\0'; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(name[i]);
        put32(bytes, offset + 8U, virtual_size);
        put32(bytes, offset + 12U, virtual_address);
        put32(bytes, offset + 16U, raw_size);
        put32(bytes, offset + 20U, raw_offset);
        put32(bytes, offset + 36U, characteristics);
    };
    write_section(0U, ".text", 0x100U, 0x1000U, 0x200U, 0x200U, 0x60000020U);
    write_section(1U, ".edata", 0x200U, 0x2000U, 0x200U, 0x400U, 0x40000040U);

    // IMAGE_EXPORT_DIRECTORY at file offset 0x400 / RVA 0x2000.
    put32(bytes, 0x40cU, 0x2070U); // Name RVA.
    put32(bytes, 0x410U, ordinal_base);
    put32(bytes, 0x414U, 4U); // NumberOfFunctions.
    put32(bytes, 0x418U, 4U); // NumberOfNames (two aliases share slot 0).
    put32(bytes, 0x41cU, 0x2040U);
    put32(bytes, 0x420U, 0x2050U);
    put32(bytes, 0x424U, 0x2060U);

    put32(bytes, 0x440U, 0x1000U);
    put32(bytes, 0x444U, 0x1010U);
    put32(bytes, 0x448U, 0x1020U);
    put32(bytes, 0x44cU, 0x20c0U); // Forwarder string within Export Directory.
    put32(bytes, 0x450U, 0x2080U);
    put32(bytes, 0x454U, 0x2090U);
    put32(bytes, 0x458U, 0x20a0U);
    put32(bytes, 0x45cU, 0x20b0U);
    put16(bytes, 0x460U, 0U);
    put16(bytes, 0x462U, 0U);
    put16(bytes, 0x464U, 1U);
    put16(bytes, 0x466U, 3U);
    put_string(bytes, 0x470U, dll_name);
    put_string(bytes, 0x480U, "N33Add");
    put_string(bytes, 0x490U, "AliasAdd");
    put_string(bytes, 0x4a0U, "N33Data");
    put_string(bytes, 0x4b0U, "N33Forwarded");
    put_string(bytes, 0x4c0U, "KERNELBASE.Sleep");
    return bytes;
}

std::vector<std::uint8_t> make_invalid_fixture(std::size_t variant) {
    auto bytes = make_pe64_dll_fixture();
    switch (variant % 25U) {
    case 0U: bytes.resize(0x20U); break;
    case 1U: bytes[0U] = 'X'; break;
    case 2U: put32(bytes, 0x3cU, 0xfffffff0U); break;
    case 3U: bytes[0x80U] = 'X'; break;
    case 4U: put16(bytes, 0x84U, 0x014cU); break;
    case 5U: put16(bytes, 0x96U, 0x0022U); break;
    case 6U: put16(bytes, 0x98U, 0x010bU); break;
    case 7U: put16(bytes, 0x94U, 112U); break;
    case 8U: put32(bytes, 0x98U + 56U, 0U); break;
    case 9U: put32(bytes, 0x98U + 60U, 0xffffffffU); break;
    case 10U: put32(bytes, 0x98U + 112U, 0U); break;
    case 11U: put32(bytes, 0x98U + 116U, 20U); break;
    case 12U: put32(bytes, 0x98U + 112U, 0x2fffU); break;
    case 13U: put16(bytes, 0x86U, 0U); break;
    case 14U: put16(bytes, 0x86U, 97U); break;
    case 15U: bytes.resize(0x1c0U); break;
    case 16U: put32(bytes, 0x188U + 20U, 0xfffffff0U); break;
    case 17U: put32(bytes, 0x188U + 40U + 12U, 0x1000U); break;
    case 18U: put32(bytes, 0x414U, 0U); break;
    case 19U: put32(bytes, 0x414U, 0xffffffffU); break;
    case 20U: put32(bytes, 0x418U, 0xffffffffU); break;
    case 21U: put32(bytes, 0x41cU, 0x2fffU); break;
    case 22U: put32(bytes, 0x420U, 0x2fffU); break;
    case 23U: put16(bytes, 0x466U, 8U); break;
    case 24U: bytes[0x480U] = 0xffU; break;
    }
    return bytes;
}

} // namespace

void run_pe64_exports_tests(int& passed, int& failed) {
    // N33A: canonical PE32+ DLL export directory, section mapping and metadata.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n33_case("N33A" + std::to_string(index + 1U) + "_pe64_export_directory_roundtrip", [index] {
            const std::uint32_t base = static_cast<std::uint32_t>(1U + (index % 5000U));
            const std::string dll = "module_" + std::to_string(index) + ".dll";
            const auto parsed = read_pe64_export_directory(make_pe64_dll_fixture(base, dll));
            n33_require(parsed.succeeded(), "valid PE32+ DLL fixture was rejected");
            n33_require(parsed.directory.dll_name == dll, "DLL name mismatch");
            n33_require(parsed.directory.ordinal_base == base, "ordinal base mismatch");
            n33_require(parsed.directory.exports.size() == 5U, "alias and ordinal-only exports were not preserved");
            n33_require(parsed.directory.exports[0U].name == "N33Add", "first named export mismatch");
            n33_require(parsed.directory.exports[0U].ordinal == base, "first export ordinal mismatch");
            n33_require(parsed.directory.exports[1U].name == "AliasAdd", "export alias mismatch");
            n33_require(parsed.directory.exports[1U].ordinal == base, "alias must share the original ordinal");
            n33_require(parsed.directory.exports[2U].name == "N33Data", "data export name mismatch");
            n33_require(parsed.directory.exports[3U].name.empty(), "ordinal-only export must have an empty name");
            n33_require(parsed.directory.exports[3U].ordinal == base + 2U, "ordinal-only export mismatch");
            n33_require(parsed.directory.exports[4U].forwarded, "forwarded export flag missing");
        }, passed, failed);
    }

    // N33B: aliases, hints, holes, ordinal-only exports and forwarded exports.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n33_case("N33B" + std::to_string(index + 1U) + "_export_symbol_mapping", [index] {
            const std::uint32_t base = static_cast<std::uint32_t>(5U + index);
            auto parsed = read_pe64_export_directory(make_pe64_dll_fixture(base));
            n33_require(parsed.succeeded(), "export mapping fixture rejected");
            const auto& exports = parsed.directory.exports;
            n33_require(exports.size() == 5U, "unexpected export count");
            n33_require(exports[0U].hint == 0U && exports[1U].hint == 1U,
                        "export-name table hints must preserve the name-table index");
            n33_require(exports[0U].function_rva == exports[1U].function_rva,
                        "aliases must resolve to the same EAT slot");
            n33_require(exports[2U].ordinal == base + 1U, "second function ordinal mismatch");
            n33_require(exports[3U].name.empty() && exports[3U].ordinal == base + 2U,
                        "ordinal-only export mapping mismatch");
            n33_require(exports[4U].forwarded && exports[4U].forwarder == "KERNELBASE.Sleep",
                        "forwarder string mismatch");
            n33_require(!exports[2U].forwarded && exports[2U].function_rva == 0x1010U,
                        "ordinary export RVA mismatch");
        }, passed, failed);
    }

    // N33C: PE exports converted to a deterministic, readable N32 COFF Import Library.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n33_case("N33C" + std::to_string(index + 1U) + "_pe_export_import_library_roundtrip", [index] {
            const auto fixture = make_pe64_dll_fixture(static_cast<std::uint32_t>(17U + index));
            const std::vector<Pe64ExportKindOverride> overrides = {
                {"N33Data", 0U, CoffImportType::Data},
                {"", static_cast<std::uint32_t>(17U + index), CoffImportType::Const},
                {"", static_cast<std::uint32_t>(19U + index), CoffImportType::Const}
            };
            const auto written = write_coff_import_library_from_pe64_dll(fixture, overrides);
            n33_require(written.succeeded(), "PE export-to-COFF import-library conversion failed" + (written.diagnostics.empty() ? std::string{} : ": " + written.diagnostics.front().code + " " + written.diagnostics.front().message));
            n33_require(written.import_count == 5U, "generated import count mismatch");
            const auto again = write_coff_import_library_from_pe64_dll(fixture, overrides);
            n33_require(again.succeeded() && again.bytes == written.bytes,
                        "same PE exports must generate byte-identical import libraries");
            const auto archive = read_coff_import_library(written.bytes);
            n33_require(archive.succeeded(), "generated COFF import library did not round-trip");
            n33_require(archive.imports.size() == 5U, "reader import count mismatch");
            const auto find_name = [&archive](const std::string& name) -> const CoffShortImportObject* {
                const auto found = std::find_if(archive.imports.begin(), archive.imports.end(),
                    [&name](const CoffShortImportObject& item) { return item.symbol_name == name; });
                return found == archive.imports.end() ? nullptr : &*found;
            };
            const auto* add = find_name("N33Add");
            const auto* data = find_name("N33Data");
            const auto* forwarded = find_name("N33Forwarded");
            n33_require(add != nullptr && add->dll_name == "n33_sample.dll" &&
                        add->name_type == CoffImportNameType::Name && add->ordinal_or_hint == 0U &&
                        add->type == CoffImportType::Const,
                        "named import metadata or ordinal-keyed kind override mismatch");
            n33_require(data != nullptr && data->type == CoffImportType::Data,
                        "DATA override was not applied");
            n33_require(forwarded != nullptr && forwarded->type == CoffImportType::Code,
                        "forwarded export must remain a normal name import");
            const auto ordinal = std::find_if(archive.imports.begin(), archive.imports.end(),
                [](const CoffShortImportObject& item) { return item.name_type == CoffImportNameType::Ordinal; });
            n33_require(ordinal != archive.imports.end() && ordinal->ordinal_or_hint == 19U + index &&
                        ordinal->symbol_name == "xfrontend_ordinal_" + std::to_string(19U + index) &&
                        ordinal->type == CoffImportType::Const,
                        "ordinal-only export, synthetic linker name or CONST override mismatch");
            const auto ambiguous = write_coff_import_library_from_pe64_dll(fixture, {
                {"N33Add", 0U, CoffImportType::Data},
                {"", static_cast<std::uint32_t>(17U + index), CoffImportType::Data}
            });
            n33_require(!ambiguous.succeeded() && !ambiguous.diagnostics.empty(),
                        "simultaneous name and ordinal overrides must be rejected as ambiguous");
        }, passed, failed);
    }

    // N33D: structural, arithmetic, table-index and string corruption rejection.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n33_case("N33D" + std::to_string(index + 1U) + "_reject_malformed_pe64_export_image", [index] {
            const auto malformed = make_invalid_fixture(index);
            const auto parsed = read_pe64_export_directory(malformed);
            n33_require(!parsed.succeeded() && !parsed.diagnostics.empty(),
                        "malformed PE32+ Export Directory was accepted");
        }, passed, failed);
    }
}
