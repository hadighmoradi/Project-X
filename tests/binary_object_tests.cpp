// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/binary_object.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void binary_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void binary_case(const std::string& name, const std::function<void()>& body,
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

void set_u16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
    bytes.at(at) = static_cast<std::uint8_t>(value & 0xffU);
    bytes.at(at + 1U) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}
void set_u32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) bytes.at(at + i) = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}
void set_u64(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i) bytes.at(at + i) = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}
std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes.at(at)) |
        (static_cast<std::uint32_t>(bytes.at(at + 1U)) << 8U) |
        (static_cast<std::uint32_t>(bytes.at(at + 2U)) << 16U) |
        (static_cast<std::uint32_t>(bytes.at(at + 3U)) << 24U);
}

ObjectFile sample_binary_object(ObjectFileFormat format, int seed) {
    ObjectFile object;
    object.object_id = "n23-object-" + std::to_string(seed);
    object.descriptor = format == ObjectFileFormat::ELF ? make_elf_x86_64_object_target()
        : (format == ObjectFileFormat::COFF ? make_coff_x86_64_object_target() : make_macho_aarch64_object_target());

    ObjectSection text;
    text.id = 11U; text.name = ".text"; text.kind = ObjectSectionKind::Text;
    text.alignment = (std::size_t{1} << (seed % 5));
    text.size = 64U; text.contents.resize(text.size);
    for (std::size_t i = 0; i < text.contents.size(); ++i)
        text.contents[i] = static_cast<std::uint8_t>((i * 13U + static_cast<unsigned>(seed)) & 0xffU);
    object.sections.push_back(text);

    ObjectSection rodata;
    rodata.id = 12U; rodata.name = ".rodata.long.payload." + std::to_string(seed);
    rodata.kind = ObjectSectionKind::ReadOnlyData; rodata.alignment = 8U;
    rodata.size = 24U; rodata.contents.resize(rodata.size);
    for (std::size_t i = 0; i < rodata.contents.size(); ++i)
        rodata.contents[i] = static_cast<std::uint8_t>((seed + static_cast<int>(i * 7U)) & 0xff);
    object.sections.push_back(rodata);

    ObjectSection data;
    data.id = 13U; data.name = ".data"; data.kind = ObjectSectionKind::Data;
    data.alignment = 8U; data.size = 16U; data.contents.assign(16U, static_cast<std::uint8_t>(seed & 0xff));
    object.sections.push_back(data);

    ObjectSection bss;
    bss.id = 14U; bss.name = ".bss"; bss.kind = ObjectSectionKind::Bss;
    bss.alignment = 16U; bss.size = 128U;
    object.sections.push_back(bss);

    ObjectSymbol function;
    function.name = "__Xv1_fn_with_a_long_native_name_" + std::to_string(seed);
    function.signature_key = "fn(i64)->i64"; function.binding = ObjectSymbolBinding::Global;
    function.kind = ObjectSymbolKind::Function; function.defined = true;
    function.section_id = text.id; function.value = 8U; function.size = 16U;
    object.symbols.push_back(function);

    ObjectSymbol object_symbol;
    object_symbol.name = "global_data_" + std::to_string(seed);
    object_symbol.signature_key = "object[24]"; object_symbol.binding = ObjectSymbolBinding::Global;
    object_symbol.kind = ObjectSymbolKind::Object; object_symbol.defined = true;
    object_symbol.section_id = rodata.id; object_symbol.value = 0U; object_symbol.size = 24U;
    object.symbols.push_back(object_symbol);

    ObjectSymbol local;
    local.name = "local_helper_" + std::to_string(seed); local.signature_key = "fn()->void";
    local.binding = ObjectSymbolBinding::Local; local.kind = ObjectSymbolKind::Function;
    local.defined = true; local.section_id = text.id; local.value = 32U; local.size = 8U;
    object.symbols.push_back(local);

    ObjectSymbol missing;
    missing.name = "external_dependency_" + std::to_string(seed);
    missing.signature_key = "fn()->void"; missing.binding = ObjectSymbolBinding::Global;
    missing.kind = ObjectSymbolKind::Function; missing.defined = false;
    object.symbols.push_back(missing);

    ObjectSymbol weak;
    weak.name = "optional_weak_dependency_" + std::to_string(seed);
    weak.signature_key = "fn()->void"; weak.binding = ObjectSymbolBinding::Weak;
    weak.kind = ObjectSymbolKind::Function; weak.defined = false;
    object.symbols.push_back(weak);

    ObjectRelocation code_relocation;
    code_relocation.section_id = text.id; code_relocation.offset = 16U;
    code_relocation.kind = format == ObjectFileFormat::COFF ? ObjectRelocationKind::CoffRelative32 : ObjectRelocationKind::PcRelative32;
    code_relocation.target_symbol = missing.name; code_relocation.addend = -4; code_relocation.width = 4U;
    object.relocations.push_back(code_relocation);

    ObjectRelocation data_relocation;
    data_relocation.section_id = data.id; data_relocation.offset = 0U;
    data_relocation.kind = format == ObjectFileFormat::COFF ? ObjectRelocationKind::CoffAddress64 : ObjectRelocationKind::Absolute64;
    data_relocation.target_symbol = function.name; data_relocation.addend = 0x1234 + seed; data_relocation.width = 8U;
    object.relocations.push_back(data_relocation);
    return object;
}

const ObjectSymbol* find_symbol(const ObjectFile& object, const std::string& name) {
    const auto found = std::find_if(object.symbols.begin(), object.symbols.end(), [&name](const ObjectSymbol& item) {
        return item.name == name;
    });
    return found == object.symbols.end() ? nullptr : &*found;
}
const ObjectSection* find_section(const ObjectFile& object, const std::string& name) {
    const auto found = std::find_if(object.sections.begin(), object.sections.end(), [&name](const ObjectSection& item) {
        return item.name == name;
    });
    return found == object.sections.end() ? nullptr : &*found;
}

} // namespace

void run_binary_object_tests(int& passed, int& failed) {
    // N23A: actual ELF64/AMD64 COFF object bytes and deterministic writer behavior.
    for (int i = 0; i < 500; ++i) {
        binary_case("N23A" + std::to_string(i + 1) + "_binary_writer_" + std::to_string(i), [=] {
            const auto format = (i % 10 == 9) ? ObjectFileFormat::MachO
                : ((i % 2) == 0 ? ObjectFileFormat::ELF : ObjectFileFormat::COFF);
            auto object = sample_binary_object(format, i);
            if (i % 10 == 7) object.sections.front().alignment = 3U;
            if (i % 10 == 8) {
                if (format == ObjectFileFormat::ELF) object.descriptor.byte_order = ObjectByteOrder::BigEndian;
                else object.sections.front().alignment = 65536U;
            }
            auto result = write_binary_object(object);
            if (format == ObjectFileFormat::MachO || i % 10 == 7 || i % 10 == 8) {
                binary_require(!result.succeeded() && !result.diagnostics.empty(),
                               "unsupported Mach-O writer profile was not rejected explicitly");
                return;
            }
            binary_require(result.succeeded(), "valid binary object failed to serialize");
            binary_require(result.bytes.size() > 64U, "serialized object is unexpectedly short");
            const auto second = write_binary_object(object);
            binary_require(second.succeeded() && second.bytes == result.bytes,
                           "binary object serialization is not deterministic");
            if (format == ObjectFileFormat::ELF) {
                binary_require(result.bytes[0] == 0x7fU && result.bytes[1] == 'E' &&
                               result.bytes[2] == 'L' && result.bytes[3] == 'F', "ELF magic is invalid");
                binary_require(result.bytes[4] == 2U && result.bytes[5] == 1U,
                               "ELF class/data encoding is not ELF64 little-endian");
                binary_require(result.bytes[16] == 1U && result.bytes[18] == 62U,
                               "ELF output is not an x86-64 relocatable object");
            } else {
                binary_require(result.bytes[0] == 0x64U && result.bytes[1] == 0x86U,
                               "COFF output does not identify AMD64 machine");
                binary_require(result.bytes[2] == 4U, "COFF section count does not match source object");
                binary_require(read_u32(result.bytes, 8U) != 0U, "COFF symbol-table pointer is missing");
            }
        }, passed, failed);
    }

    // N23B: reader round-trip for sections, BSS, long names and symbol binding.
    for (int i = 0; i < 500; ++i) {
        binary_case("N23B" + std::to_string(i + 1) + "_binary_reader_roundtrip_" + std::to_string(i), [=] {
            const auto format = (i % 2) == 0 ? ObjectFileFormat::ELF : ObjectFileFormat::COFF;
            const auto original = sample_binary_object(format, i + 1000);
            const auto written = write_binary_object(original);
            binary_require(written.succeeded(), "writer rejected round-trip fixture");
            const auto parsed = read_binary_object(written.bytes);
            binary_require(parsed.succeeded(), "reader rejected writer-produced object");
            binary_require(parsed.object_file.sections.size() == original.sections.size(),
                           "round-trip section count changed");
            binary_require(parsed.object_file.symbols.size() == original.symbols.size(),
                           "round-trip symbol count changed");
            binary_require(parsed.object_file.relocations.size() == original.relocations.size(),
                           "round-trip relocation count changed");
            const auto* text = find_section(parsed.object_file, ".text");
            const auto* bss = find_section(parsed.object_file, ".bss");
            const auto* rodata = find_section(parsed.object_file, original.sections[1].name);
            binary_require(text != nullptr && text->kind == ObjectSectionKind::Text && text->contents.size() == 64U,
                           "text section did not round-trip");
            binary_require(bss != nullptr && bss->kind == ObjectSectionKind::Bss && bss->size == 128U && bss->contents.empty(),
                           "BSS logical size or no-file-content rule did not round-trip");
            binary_require(rodata != nullptr && rodata->kind == ObjectSectionKind::ReadOnlyData && rodata->contents.size() == 24U,
                           "read-only data or long section name did not round-trip");
            const auto* function = find_symbol(parsed.object_file, original.symbols[0].name);
            const auto* local = find_symbol(parsed.object_file, original.symbols[2].name);
            const auto* undefined = find_symbol(parsed.object_file, original.symbols[3].name);
            const auto* weak = find_symbol(parsed.object_file, original.symbols[4].name);
            binary_require(function != nullptr && function->defined && function->kind == ObjectSymbolKind::Function,
                           "defined function symbol did not round-trip");
            binary_require(local != nullptr && local->binding == ObjectSymbolBinding::Local,
                           "local symbol binding did not round-trip");
            binary_require(undefined != nullptr && !undefined->defined,
                           "undefined external symbol did not round-trip");
            binary_require(weak != nullptr && !weak->defined && weak->binding == ObjectSymbolBinding::Weak,
                           "weak undefined symbol binding did not round-trip");
        }, passed, failed);
    }

    // N23C: relocation encodings, explicit ELF addends and implicit COFF addends.
    for (int i = 0; i < 500; ++i) {
        binary_case("N23C" + std::to_string(i + 1) + "_binary_relocation_addend_" + std::to_string(i), [=] {
            const auto format = (i % 2) == 0 ? ObjectFileFormat::ELF : ObjectFileFormat::COFF;
            auto original = sample_binary_object(format, i + 2000);
            original.relocations[0].addend = -1000 - i;
            original.relocations[1].addend = 5000 + i;
            const auto written = write_binary_object(original);
            binary_require(written.succeeded(), "relocation fixture failed to serialize");
            const auto parsed = read_binary_object(written.bytes);
            binary_require(parsed.succeeded(), "relocation fixture failed to parse");
            binary_require(parsed.object_file.relocations.size() == 2U, "relocation count changed");
            const auto code = std::find_if(parsed.object_file.relocations.begin(), parsed.object_file.relocations.end(),
                [](const ObjectRelocation& item) { return item.width == 4U; });
            const auto data = std::find_if(parsed.object_file.relocations.begin(), parsed.object_file.relocations.end(),
                [](const ObjectRelocation& item) { return item.width == 8U; });
            binary_require(code != parsed.object_file.relocations.end() && data != parsed.object_file.relocations.end(),
                           "relocation widths were not preserved");
            binary_require(code->addend == original.relocations[0].addend,
                           "32-bit relocation addend did not round-trip");
            binary_require(data->addend == original.relocations[1].addend,
                           "64-bit relocation addend did not round-trip");
            binary_require(code->target_symbol == original.relocations[0].target_symbol &&
                           data->target_symbol == original.relocations[1].target_symbol,
                           "relocation symbol references changed during round-trip");
        }, passed, failed);
    }

    // N23D: hostile/truncated binary inputs are rejected without out-of-range access.
    for (int i = 0; i < 500; ++i) {
        binary_case("N23D" + std::to_string(i + 1) + "_binary_reader_corruption_" + std::to_string(i), [=] {
            const auto format = (i % 2) == 0 ? ObjectFileFormat::ELF : ObjectFileFormat::COFF;
            const auto original = sample_binary_object(format, i + 3000);
            auto written = write_binary_object(original);
            binary_require(written.succeeded(), "corruption fixture failed to serialize");
            auto bytes = written.bytes;
            switch (i % 10) {
            case 0:
                bytes.resize(8U);
                break;
            case 1:
                if (format == ObjectFileFormat::ELF) bytes[4] = 1U;
                else set_u16(bytes, 0U, 0x014cU);
                break;
            case 2:
                if (format == ObjectFileFormat::ELF) set_u64(bytes, 40U, 0xfffffff0ULL);
                else set_u32(bytes, 8U, 0xfffffff0U);
                break;
            case 3:
                if (format == ObjectFileFormat::ELF) set_u16(bytes, 62U, 0xffffU);
                else bytes.resize(20U);
                break;
            case 4:
                if (format == ObjectFileFormat::ELF) set_u16(bytes, 58U, 16U);
                else set_u32(bytes, 12U, 0xfffffff0U);
                break;
            case 5:
                if (format == ObjectFileFormat::ELF) {
                    set_u16(bytes, 18U, 3U);
                } else {
                    const auto section_reloc_pointer = 20U + 24U;
                    set_u32(bytes, section_reloc_pointer, 0xfffffff0U);
                }
                break;
            case 6:
                if (format == ObjectFileFormat::ELF) {
                    const auto shoff = static_cast<std::size_t>(bytes[40]) |
                        (static_cast<std::size_t>(bytes[41]) << 8U) |
                        (static_cast<std::size_t>(bytes[42]) << 16U) |
                        (static_cast<std::size_t>(bytes[43]) << 24U);
                    if (shoff + 64U < bytes.size()) set_u32(bytes, shoff + 64U + 4U, 0xffffffffU);
                    else bytes[5] = 0U;
                } else {
                    const auto symbol_pointer = read_u32(bytes, 8U);
                    const auto symbol_count = read_u32(bytes, 12U);
                    const auto string_size_at = static_cast<std::size_t>(symbol_pointer) + static_cast<std::size_t>(symbol_count) * 18U;
                    if (string_size_at + 4U <= bytes.size()) set_u32(bytes, string_size_at, 0xfffffff0U);
                }
                break;
            case 7:
                if (format == ObjectFileFormat::ELF) bytes[0] = 0U;
                else set_u16(bytes, 2U, 0xffffU);
                break;
            case 8:
                if (format == ObjectFileFormat::ELF) set_u16(bytes, 16U, 2U);
                else set_u32(bytes, 20U + 16U, 0xfffffff0U);
                break;
            default:
                bytes[0] = 0x55U; bytes[1] = 0xAAU; bytes[2] = 0x55U; bytes[3] = 0xAAU;
                break;
            }
            const auto parsed = read_binary_object(bytes);
            binary_require(!parsed.succeeded(), "malformed or unsupported binary object was accepted");
        }, passed, failed);
    }
}
