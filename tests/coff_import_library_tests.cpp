// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/coff_import_library.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

void n32_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n32_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::uint16_t n32_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n32_require(offset <= bytes.size() && bytes.size() - offset >= 2U,
                "test attempted to read a truncated u16 field");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t n32_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n32_require(offset <= bytes.size() && bytes.size() - offset >= 4U,
                "test attempted to read a truncated u32 field");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

CoffImportSymbol n32_import(std::size_t seed, CoffImportType type,
                            CoffImportNameType name_type) {
    CoffImportSymbol item;
    item.dll_name = "n32_module_" + std::to_string(seed % 19U) + ".dll";
    item.symbol_name = "N32_Export_" + std::to_string(seed);
    item.type = type;
    item.name_type = name_type;
    item.ordinal_or_hint = name_type == CoffImportNameType::Ordinal
        ? static_cast<std::uint16_t>(1U + (seed % 65535U))
        : static_cast<std::uint16_t>((seed * 37U) & 0xffffU);
    item.timestamp = static_cast<std::uint32_t>(seed * 101U);
    item.member_name = "n32/imports/long_member_group_" +
        std::to_string(seed % 19U) + ".dll";
    return item;
}

bool n32_is_short_import_member(const ArchiveContainerMember& value) {
    return value.contents.size() >= 4U && value.contents[0U] == 0U &&
        value.contents[1U] == 0U && value.contents[2U] == 0xffU &&
        value.contents[3U] == 0xffU;
}

std::string n32_library_stem(const std::string& dll_name) {
    const auto separator = dll_name.find_last_of("/\\");
    const std::string basename = separator == std::string::npos
        ? dll_name : dll_name.substr(separator + 1U);
    const auto extension = basename.find_last_of('.');
    return extension == std::string::npos || extension == 0U
        ? basename : basename.substr(0U, extension);
}

std::vector<std::uint8_t> n32_member_bytes(const CoffImportSymbol& item) {
    const auto written = write_coff_import_library({item});
    n32_require(written.succeeded(), "could not prepare valid import object fixture");
    const auto archive = read_archive_container(written.bytes);
    n32_require(archive.succeeded(), "could not read import object fixture archive");
    const auto member = std::find_if(archive.archive.members.begin(), archive.archive.members.end(),
        [&item](const ArchiveContainerMember& value) {
            return value.name == item.member_name && n32_is_short_import_member(value);
        });
    n32_require(member != archive.archive.members.end(), "could not locate short-import fixture member");
    return member->contents;
}

bool n32_has_index_symbol(const ArchiveContainer& archive, const std::string& name,
                          std::size_t member_index = std::numeric_limits<std::size_t>::max()) {
    return std::any_of(archive.symbol_index.begin(), archive.symbol_index.end(),
        [&name, member_index](const ArchiveSymbolIndexEntry& entry) {
            return entry.symbol_name == name &&
                (member_index == std::numeric_limits<std::size_t>::max() || entry.member_index == member_index);
        });
}

} // namespace

void run_coff_import_library_tests(int& passed, int& failed) {
    // N32A: exact short-import header layout and round-trip of symbol metadata.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n32_case("N32A" + std::to_string(index + 1U) + "_short_import_header_roundtrip", [index] {
            const auto name_type = static_cast<CoffImportNameType>(index % 4U);
            const auto item = n32_import(index + 1U, CoffImportType::Code, name_type);
            const auto written = write_coff_import_library({item});
            n32_require(written.succeeded(), "valid AMD64 short import must be emitted");
            n32_require(written.import_count == 1U, "writer import count mismatch");
            const auto archive = read_archive_container(written.bytes);
            n32_require(archive.succeeded() && archive.archive.format == ArchiveFormat::CoffLib,
                        "short import archive did not use COFF LIB format");
            n32_require(archive.archive.members.size() == 4U,
                        "single-DLL import library must include descriptor, terminators, and short import");
            const auto raw_member = std::find_if(archive.archive.members.begin(), archive.archive.members.end(),
                [&item](const ArchiveContainerMember& value) {
                    return value.name == item.member_name && n32_is_short_import_member(value);
                });
            n32_require(raw_member != archive.archive.members.end(), "short-import archive member missing");
            const auto& raw = raw_member->contents;
            n32_require(raw.size() == 20U + item.symbol_name.size() + item.dll_name.size() + 2U,
                        "short import object size mismatch");
            n32_require(n32_u16(raw, 0U) == 0U, "Sig1 must be IMAGE_FILE_MACHINE_UNKNOWN");
            n32_require(n32_u16(raw, 2U) == 0xffffU, "Sig2 must be 0xffff");
            n32_require(n32_u16(raw, 4U) == 0U, "short import Version must be zero");
            n32_require(n32_u16(raw, 6U) == CoffMachineAmd64, "short import machine mismatch");
            n32_require(n32_u32(raw, 8U) == item.timestamp, "short import timestamp mismatch");
            n32_require(n32_u32(raw, 12U) == item.symbol_name.size() + item.dll_name.size() + 2U,
                        "SizeOfData must cover the two NUL-terminated strings");
            n32_require(n32_u16(raw, 16U) == item.ordinal_or_hint, "Ordinal/Hint mismatch");
            n32_require(n32_u16(raw, 18U) == (static_cast<std::uint16_t>(item.type) |
                        (static_cast<std::uint16_t>(item.name_type) << 2U)),
                        "TypeInfo bit packing mismatch");
            const auto decoded = read_coff_short_import_object(raw);
            n32_require(decoded.succeeded(), "short import reader rejected writer output");
            n32_require(decoded.import_object.machine == CoffMachineAmd64, "reader machine mismatch");
            n32_require(decoded.import_object.timestamp == item.timestamp, "reader timestamp mismatch");
            n32_require(decoded.import_object.symbol_name == item.symbol_name, "reader import symbol mismatch");
            n32_require(decoded.import_object.dll_name == item.dll_name, "reader DLL name mismatch");
            n32_require(decoded.import_object.type == item.type, "reader import type mismatch");
            n32_require(decoded.import_object.name_type == item.name_type, "reader name type mismatch");
            n32_require(decoded.import_object.ordinal_or_hint == item.ordinal_or_hint,
                        "reader ordinal/hint mismatch");
            n32_require(decoded.import_object.member_name == "", "standalone short-import parser must not invent a member name");

            const std::string library_stem = n32_library_stem(item.dll_name);
            const auto descriptor_index = std::find_if(archive.archive.symbol_index.begin(),
                archive.archive.symbol_index.end(), [&library_stem](const ArchiveSymbolIndexEntry& value) {
                    return value.symbol_name == "__IMPORT_DESCRIPTOR_" + library_stem;
                });
            n32_require(descriptor_index != archive.archive.symbol_index.end(),
                        "COFF import descriptor index entry missing");
            n32_require(descriptor_index->member_index < archive.archive.members.size(),
                        "COFF import descriptor member index is out of range");
            const auto& descriptor_bytes = archive.archive.members[descriptor_index->member_index].contents;
            n32_require(n32_u16(descriptor_bytes, 0U) == CoffMachineAmd64,
                        "import descriptor COFF object must target AMD64");
            const auto descriptor_symbol_offset = static_cast<std::size_t>(n32_u32(descriptor_bytes, 8U));
            const auto descriptor_symbol_count = n32_u32(descriptor_bytes, 12U);
            n32_require(descriptor_symbol_count == 7U,
                        "import descriptor must contain the seven canonical COFF symbols");
            const std::vector<std::uint8_t> expected_storage_classes = {2U, 104U, 3U, 104U, 104U, 2U, 2U};
            for (std::size_t symbol_index = 0U; symbol_index < expected_storage_classes.size(); ++symbol_index) {
                const auto storage_class_offset = descriptor_symbol_offset + symbol_index * 18U + 16U;
                n32_require(storage_class_offset < descriptor_bytes.size(),
                            "import descriptor symbol table is truncated");
                n32_require(descriptor_bytes[storage_class_offset] == expected_storage_classes[symbol_index],
                            "import descriptor COFF symbol storage class does not match PE/COFF import-library convention");
            }
        }, passed, failed);
    }

    // N32B: the linker-visible symbol mapping for CODE, DATA and CONST imports.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n32_case("N32B" + std::to_string(index + 1U) + "_import_type_linker_symbols", [index] {
            const auto code = n32_import(index * 3U + 1U, CoffImportType::Code,
                static_cast<CoffImportNameType>(index % 4U));
            const auto data = n32_import(index * 3U + 2U, CoffImportType::Data,
                static_cast<CoffImportNameType>((index + 1U) % 4U));
            const auto constant = n32_import(index * 3U + 3U, CoffImportType::Const,
                static_cast<CoffImportNameType>((index + 2U) % 4U));
            const auto written = write_coff_import_library({code, data, constant});
            n32_require(written.succeeded(), "three valid import types must generate a library");
            const auto read = read_coff_import_library(written.bytes);
            n32_require(read.succeeded(), "import library reader rejected generated CODE/DATA/CONST archive");
            n32_require(read.imports.size() == 3U, "three import objects expected");
            n32_require(read.archive.symbol_index.size() == 12U,
                        "five import symbols plus three per-DLL descriptor support symbols are required");
            n32_require(n32_has_index_symbol(read.archive, code.symbol_name), "CODE thunk symbol missing from index");
            n32_require(n32_has_index_symbol(read.archive, "__imp_" + code.symbol_name), "CODE IAT symbol missing from index");
            n32_require(!n32_has_index_symbol(read.archive, data.symbol_name), "DATA must not define a callable thunk");
            n32_require(n32_has_index_symbol(read.archive, "__imp_" + data.symbol_name), "DATA IAT symbol missing from index");
            n32_require(n32_has_index_symbol(read.archive, constant.symbol_name), "CONST public alias missing from index");
            n32_require(n32_has_index_symbol(read.archive, "__imp_" + constant.symbol_name), "CONST IAT symbol missing from index");
            for (const auto* imported : {&code, &data, &constant}) {
                const std::string stem = n32_library_stem(imported->dll_name);
                n32_require(n32_has_index_symbol(read.archive, "__IMPORT_DESCRIPTOR_" + stem),
                            "DLL import descriptor symbol missing from the LIB index");
                n32_require(n32_has_index_symbol(read.archive,
                    std::string(1U, static_cast<char>(0x7fU)) + stem + "_NULL_THUNK_DATA"),
                    "DLL null-thunk support symbol missing from the LIB index");
            }
            n32_require(n32_has_index_symbol(read.archive, "__NULL_IMPORT_DESCRIPTOR"),
                        "global null import descriptor symbol missing from the LIB index");
            const auto code_symbols = coff_import_linker_symbols(read.imports[0]);
            const auto data_symbols = coff_import_linker_symbols(read.imports[1]);
            const auto const_symbols = coff_import_linker_symbols(read.imports[2]);
            n32_require(code_symbols.size() == 2U && data_symbols.size() == 1U && const_symbols.size() == 2U,
                        "public symbol mapping cardinality mismatch");
            n32_require(code_symbols[0] == "__imp_" + code.symbol_name && code_symbols[1] == code.symbol_name,
                        "CODE linker symbol order/content mismatch");
            n32_require(data_symbols[0] == "__imp_" + data.symbol_name,
                        "DATA linker symbol mapping mismatch");
        }, passed, failed);
    }

    // N32C: malformed short-import headers and malformed string payloads.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n32_case("N32C" + std::to_string(index + 1U) + "_reject_malformed_short_import", [index] {
            const auto mode = index % 15U;
            auto fixture = n32_import(index + 9000U, CoffImportType::Code,
                mode == 13U ? CoffImportNameType::Ordinal : CoffImportNameType::Name);
            auto bytes = n32_member_bytes(fixture);
            switch (mode) {
                case 0U: bytes[0U] = 1U; break; // Sig1.
                case 1U: bytes[2U] = 0xfeU; break; // Sig2.
                case 2U: bytes[4U] = 1U; break; // Version.
                case 3U: bytes[6U] = 0x4cU; bytes[7U] = 0x01U; break; // I386 machine.
                case 4U: bytes[12U] = 0xffU; bytes[13U] = 0xffU; bytes[14U] = 0xffU; bytes[15U] = 0x7fU; break; // Size.
                case 5U: bytes[19U] = static_cast<std::uint8_t>(bytes[19U] | 0x20U); break; // Reserved bits.
                case 6U: bytes[18U] = static_cast<std::uint8_t>((bytes[18U] & 0xfcU) | 3U); break; // Type 3.
                case 7U: bytes[18U] = static_cast<std::uint8_t>((bytes[18U] & 0xe3U) | 0x10U); break; // Name type 4.
                case 8U: bytes[20U + fixture.symbol_name.size()] = static_cast<std::uint8_t>('X'); break; // Missing first NUL.
                case 9U: bytes.back() = static_cast<std::uint8_t>('X'); break; // Missing DLL NUL.
                case 10U: bytes[20U] = 0U; break; // Empty import name.
                case 11U: bytes[21U + fixture.symbol_name.size()] = 0U; break; // Empty DLL name.
                case 12U: bytes.push_back(0x5aU); break; // Unexpected trailing data.
                case 13U: bytes[16U] = 0U; bytes[17U] = 0U; break; // Zero ordinal.
                case 14U: bytes[20U] = 0x7fU; break; // Non-printable ASCII.
                default: throw std::runtime_error("unreachable malformed-input test variant");
            }
            const auto decoded = read_coff_short_import_object(bytes);
            n32_require(!decoded.succeeded(), "malformed short-import object was accepted");
            n32_require(!decoded.diagnostics.empty(), "malformed short-import rejection must include a diagnostic");
        }, passed, failed);
    }

    // N32D: multi-member import libraries, stable bytes, long member names and exact symbol indexes.
    for (std::size_t index = 0U; index < 500U; ++index) {
        n32_case("N32D" + std::to_string(index + 1U) + "_import_library_archive_index_roundtrip", [index] {
            const std::size_t count = 1U + (index % 16U);
            std::vector<CoffImportSymbol> imports;
            imports.reserve(count);
            for (std::size_t item_index = 0U; item_index < count; ++item_index) {
                const auto type = static_cast<CoffImportType>((index + item_index) % 3U);
                const auto name_type = static_cast<CoffImportNameType>((index * 3U + item_index) % 4U);
                imports.push_back(n32_import(index * 31U + item_index + 20000U, type, name_type));
            }
            const auto first = write_coff_import_library(imports);
            const auto second = write_coff_import_library(imports);
            n32_require(first.succeeded() && second.succeeded(), "valid multi-member import library must be emitted");
            n32_require(first.bytes == second.bytes, "zero-timestamp import library generation must be deterministic");
            n32_require(first.import_count == count, "multi-member import count mismatch");
            const auto read = read_coff_import_library(first.bytes);
            n32_require(read.succeeded(), "multi-member import library failed round-trip/index validation");
            n32_require(read.imports.size() == count, "multi-member read count mismatch");
            std::set<std::string> expected_symbols;
            for (std::size_t item_index = 0U; item_index < count; ++item_index) {
                const auto symbols = coff_import_linker_symbols(read.imports[item_index]);
                for (const auto& symbol : symbols) expected_symbols.insert(symbol);
                n32_require(read.imports[item_index].symbol_name == imports[item_index].symbol_name,
                            "round-trip member order or symbol name mismatch");
                n32_require(read.imports[item_index].member_name == imports[item_index].member_name,
                            "long/short member name round-trip mismatch");
            }
            std::set<std::string> library_stems;
            for (const auto& import : read.imports) {
                const auto separator = import.dll_name.find_last_of("/\\");
                const std::string basename = separator == std::string::npos
                    ? import.dll_name : import.dll_name.substr(separator + 1U);
                const auto extension = basename.find_last_of('.');
                const std::string stem = extension == std::string::npos || extension == 0U
                    ? basename : basename.substr(0U, extension);
                library_stems.insert(stem);
            }
            expected_symbols.insert("__NULL_IMPORT_DESCRIPTOR");
            for (const auto& stem : library_stems) {
                expected_symbols.insert("__IMPORT_DESCRIPTOR_" + stem);
                expected_symbols.insert(std::string(1U, static_cast<char>(0x7fU)) + stem + "_NULL_THUNK_DATA");

                const auto descriptor_entry = std::find_if(read.archive.symbol_index.begin(),
                    read.archive.symbol_index.end(), [&stem](const ArchiveSymbolIndexEntry& entry) {
                        return entry.symbol_name == "__IMPORT_DESCRIPTOR_" + stem;
                    });
                const auto thunk_entry = std::find_if(read.archive.symbol_index.begin(),
                    read.archive.symbol_index.end(), [&stem](const ArchiveSymbolIndexEntry& entry) {
                        return entry.symbol_name == std::string(1U, static_cast<char>(0x7fU)) +
                            stem + "_NULL_THUNK_DATA";
                    });
                n32_require(descriptor_entry != read.archive.symbol_index.end() &&
                            thunk_entry != read.archive.symbol_index.end(),
                            "per-DLL descriptor and null-thunk index entries must exist");
                const auto expected_member_name = std::find_if(read.imports.begin(), read.imports.end(),
                    [&stem](const CoffShortImportObject& import_object) {
                        const auto separator = import_object.dll_name.find_last_of("/\\");
                        const std::string basename = separator == std::string::npos
                            ? import_object.dll_name : import_object.dll_name.substr(separator + 1U);
                        const auto extension = basename.find_last_of('.');
                        const std::string current_stem = extension == std::string::npos || extension == 0U
                            ? basename : basename.substr(0U, extension);
                        return current_stem == stem;
                    });
                n32_require(expected_member_name != read.imports.end(),
                            "an import member must exist for each DLL group");
                n32_require(read.archive.members[descriptor_entry->member_index].name == expected_member_name->member_name &&
                            read.archive.members[thunk_entry->member_index].name == expected_member_name->member_name,
                            "descriptor, null thunk, and short-import members for one DLL must share the same archive name");
            }
            n32_require(expected_symbols.size() == read.archive.symbol_index.size(),
                        "linker index must cover import thunks and all per-DLL descriptor support symbols");
            n32_require(read.archive.members.size() == count + (library_stems.size() * 2U) + 1U,
                        "archive member count must include each descriptor, one null descriptor, each null thunk, and imports");
            for (const auto& entry : read.archive.symbol_index) {
                n32_require(expected_symbols.count(entry.symbol_name) == 1U,
                            "archive linker index contains an unexpected symbol");
                n32_require(entry.member_index < read.archive.members.size(),
                            "archive symbol index member is out of range");
            }
            CoffImportSymbol inconsistent_a;
            inconsistent_a.dll_name = "same_group.dll";
            inconsistent_a.symbol_name = "same_group_a";
            inconsistent_a.member_name = "group_one.dll";
            CoffImportSymbol inconsistent_b = inconsistent_a;
            inconsistent_b.symbol_name = "same_group_b";
            inconsistent_b.member_name = "group_two.dll";
            n32_require(!write_coff_import_library({inconsistent_a, inconsistent_b}).succeeded(),
                        "one DLL group must reject inconsistent archive-member names");
            CoffImportSymbol colliding_group_a;
            colliding_group_a.dll_name = "first_group.dll";
            colliding_group_a.symbol_name = "first_group_symbol";
            colliding_group_a.member_name = "shared_archive_name.dll";
            CoffImportSymbol colliding_group_b;
            colliding_group_b.dll_name = "second_group.dll";
            colliding_group_b.symbol_name = "second_group_symbol";
            colliding_group_b.member_name = "shared_archive_name.dll";
            n32_require(!write_coff_import_library({colliding_group_a, colliding_group_b}).succeeded(),
                        "different DLL groups must reject a shared archive-member name");

            const auto other_format = write_archive_container(ArchiveFormat::GnuAr,
                {ArchiveContainerMember{"plain.o", {1U, 2U, 3U}, {"plain"}, 0U}}, true);
            n32_require(other_format.succeeded(), "GNU ar negative fixture generation failed");
            n32_require(!read_coff_import_library(other_format.bytes).succeeded(),
                        "GNU ar must not be accepted as a COFF import library");
        }, passed, failed);
    }
}
