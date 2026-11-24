// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/elf64_shared_object.hpp"
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

void n27_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n27_case(const std::string& name, const std::function<void()>& body,
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

std::uint16_t n27_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n27_require(offset <= bytes.size() && bytes.size() - offset >= 2U, "u16 outside ELF bytes");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t n27_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n27_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "u32 outside ELF bytes");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t n27_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n27_require(offset <= bytes.size() && bytes.size() - offset >= 8U, "u64 outside ELF bytes");
    std::uint64_t value = 0U;
    for (unsigned i = 0U; i < 8U; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

std::int32_t n27_i32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::int32_t>(n27_u32(bytes, offset));
}

std::uint64_t n27_dynamic_value(const Elf64SharedObjectImage& image, std::int64_t tag) {
    for (const auto& item : image.dynamic_entries) if (item.tag == tag) return item.value;
    throw std::runtime_error("dynamic tag not found");
}

std::string n27_string_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n27_require(offset < bytes.size(), "dynamic string offset outside ELF file");
    std::string value;
    while (offset < bytes.size() && bytes[offset] != 0U) value.push_back(static_cast<char>(bytes[offset++]));
    n27_require(offset < bytes.size(), "dynamic string is not NUL terminated");
    return value;
}

Elf64SharedObjectSpec n27_basic_spec() {
    Elf64SharedObjectSpec spec;
    spec.soname = "libx_n27.so";
    spec.text = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U};
    spec.exports.push_back(Elf64ExportedSymbol{
        "x_n27_answer", Elf64SharedSection::Text, 0U, spec.text.size(), Elf64DynamicSymbolKind::Function});
    return spec;
}

bool n27_has_diag(const std::vector<Diagnostic>& items, const std::string& code) {
    return std::any_of(items.begin(), items.end(), [&code](const Diagnostic& item) { return item.code == code; });
}

void n27_expect_failure(const Elf64SharedObjectSpec& spec, const std::string& code) {
    const auto result = write_elf64_shared_object(spec);
    n27_require(!result.succeeded(), "invalid dynamic-import specification unexpectedly succeeded");
    n27_require(n27_has_diag(result.diagnostics, code), "expected diagnostic " + code + " missing");
    n27_require(result.image.bytes.empty(), "failed shared writer must not expose partial ELF image");
}

std::uint32_t n27_sysv_hash(const std::string& name) {
    std::uint32_t hash = 0U;
    for (unsigned char ch : name) {
        hash = (hash << 4U) + ch;
        const std::uint32_t high = hash & 0xf0000000U;
        if (high != 0U) hash ^= high >> 24U;
        hash &= ~high;
    }
    return hash;
}

} // namespace

void run_elf64_dynamic_import_tests(int& passed, int& failed) {
    // N27A: DT_NEEDED, undefined dynsym records, dynstr offsets and SysV hash lookup.
    for (int index = 0; index < 500; ++index) {
        n27_case("N27A" + std::to_string(index + 1) + "_needed_import_dynsym_hash", [index] {
            auto spec = n27_basic_spec();
            const std::size_t import_count = static_cast<std::size_t>(1 + (index % 7));
            const std::size_t dependency_count = static_cast<std::size_t>(1 + (index % 3));
            for (std::size_t i = 0U; i < import_count; ++i) {
                spec.imports.push_back(Elf64ImportedSymbol{
                    "x_external_" + std::to_string((import_count - i) * 17U + i),
                    (i % 2U) == 0U ? Elf64DynamicSymbolKind::Function : Elf64DynamicSymbolKind::Object});
            }
            for (std::size_t i = 0U; i < dependency_count; ++i)
                spec.needed_libraries.push_back("libfixture_" + std::to_string(i) + ".so");
            auto sorted_imports = spec.imports;
            std::sort(sorted_imports.begin(), sorted_imports.end(), [](const auto& left, const auto& right) {
                return left.name < right.name;
            });
            const auto result = write_elf64_shared_object(spec);
            n27_require(result.succeeded(), "valid imported dynamic symbols should serialize");
            n27_require(result.image.imported_symbol_count == import_count, "import count mismatch");
            n27_require(result.image.needed_library_count == dependency_count, "DT_NEEDED count mismatch");
            n27_require(result.image.dynamic_entries.size() == 10U + dependency_count, "dynamic table size mismatch");
            n27_require(result.image.dynamic_entries.back().tag == 0 && result.image.dynamic_entries.back().value == 0U,
                        "dynamic table must terminate with DT_NULL");
            const auto& bytes = result.image.bytes;
            const auto strtab = static_cast<std::size_t>(n27_dynamic_value(result.image, 5));
            const auto symtab = static_cast<std::size_t>(n27_dynamic_value(result.image, 6));
            const auto hash = static_cast<std::size_t>(n27_dynamic_value(result.image, 4));
            for (std::size_t i = 0U; i < dependency_count; ++i) {
                n27_require(result.image.dynamic_entries[i].tag == 1, "dependency entries must use DT_NEEDED");
                n27_require(n27_string_at(bytes, strtab + static_cast<std::size_t>(result.image.dynamic_entries[i].value)) ==
                            spec.needed_libraries[i], "DT_NEEDED name offset mismatch");
            }
            n27_require(n27_u32(bytes, hash + 4U) == import_count + spec.exports.size() + 1U,
                        "SysV hash nchain must cover exports, imports and null symbol");
            for (std::size_t i = 0U; i < import_count; ++i) {
                const std::size_t symbol_index = spec.exports.size() + i + 1U;
                const std::size_t symbol = symtab + symbol_index * 24U;
                const std::string name = n27_string_at(bytes, strtab + n27_u32(bytes, symbol));
                n27_require(name == sorted_imports[i].name, "undefined dynsym name mismatch");
                n27_require(n27_u16(bytes, symbol + 6U) == 0U, "imported dynamic symbol must have SHN_UNDEF");
                n27_require(n27_u64(bytes, symbol + 8U) == 0U && n27_u64(bytes, symbol + 16U) == 0U,
                            "undefined dynamic symbol value and size must be zero");
                n27_require(bytes[symbol + 4U] == static_cast<std::uint8_t>(0x10U |
                    static_cast<std::uint8_t>(sorted_imports[i].kind)),
                    "imported symbol binding/type encoding mismatch");
                const std::size_t bucket_count = n27_u32(bytes, hash);
                n27_require(bucket_count > 0U, "SysV hash must expose at least one bucket");
                const std::size_t bucket = n27_sysv_hash(name) % bucket_count;
                std::uint32_t chain = n27_u32(bytes, hash + 8U + bucket * 4U);
                bool found = false;
                for (std::size_t step = 0U; chain != 0U && step <= import_count + spec.exports.size(); ++step) {
                    n27_require(chain <= import_count + spec.exports.size(), "SysV hash chain index exceeds dynsym");
                    if (chain == symbol_index) found = true;
                    chain = n27_u32(bytes, hash + 8U + bucket_count * 4U + chain * 4U);
                }
                n27_require(found, "undefined import must be discoverable via SysV hash");
            }
        }, passed, failed);
    }

    // N27B: one GOT slot and R_X86_64_GLOB_DAT per imported symbol.
    for (int index = 0; index < 500; ++index) {
        n27_case("N27B" + std::to_string(index + 1) + "_got_glob_dat_relocations", [index] {
            auto spec = n27_basic_spec();
            spec.data.assign(static_cast<std::size_t>(8 + (index % 5) * 8), 0x5aU);
            spec.bss_size = static_cast<std::size_t>(8 + (index % 4) * 8);
            spec.relative_relocations.push_back(Elf64RelativeRelocation{
                spec.data.size(), Elf64SharedSection::Data, spec.data.size()});
            const std::size_t import_count = static_cast<std::size_t>(1 + (index % 6));
            spec.needed_libraries = {"libc.so.6"};
            for (std::size_t i = 0U; i < import_count; ++i)
                spec.imports.push_back(Elf64ImportedSymbol{"external_" + std::to_string(i), Elf64DynamicSymbolKind::Function});
            const auto result = write_elf64_shared_object(spec);
            n27_require(result.succeeded(), "valid GOT and GLOB_DAT layout should succeed");
            n27_require(result.image.got_size == import_count * 8U, "GOT size must have one pointer per import");
            n27_require((result.image.got_address & 7U) == 0U, "GOT base must be pointer aligned");
            n27_require(result.image.symbol_relocation_count == import_count, "symbol relocation count mismatch");
            n27_require(n27_dynamic_value(result.image, 8) == (import_count + 1U) * 24U,
                        "DT_RELASZ must include BSS RELATIVE and GLOB_DAT records");
            const auto rela = static_cast<std::size_t>(n27_dynamic_value(result.image, 7));
            const auto symtab = static_cast<std::size_t>(n27_dynamic_value(result.image, 6));
            const std::size_t got_offset = (spec.data.size() + 7U) & ~static_cast<std::size_t>(7U);
            const std::uint64_t bss_address = result.image.data_address + got_offset + import_count * 8U;
            n27_require(n27_u64(result.image.bytes, rela) == bss_address,
                        "RELATIVE relocation into BSS must be mapped after the reserved GOT");
            n27_require(n27_u64(result.image.bytes, rela + 8U) == 8U,
                        "BSS relocation must remain R_X86_64_RELATIVE with symbol zero");
            n27_require(n27_u64(result.image.bytes, rela + 16U) == bss_address,
                        "BSS RELATIVE addend must use the GOT-adjusted image address");
            for (std::size_t i = 0U; i < import_count; ++i) {
                const std::size_t entry = rela + (i + 1U) * 24U;
                const std::size_t symbol_index = spec.exports.size() + i + 1U;
                n27_require(n27_u64(result.image.bytes, entry) == result.image.got_address + i * 8U,
                            "GLOB_DAT destination must address matching GOT slot");
                n27_require(n27_u64(result.image.bytes, entry + 8U) ==
                            ((static_cast<std::uint64_t>(symbol_index) << 32U) | 6U),
                            "r_info must encode imported symbol and R_X86_64_GLOB_DAT");
                n27_require(n27_u64(result.image.bytes, entry + 16U) == 0U, "GLOB_DAT RELA addend must be zero");
                n27_require(n27_u16(result.image.bytes, symtab + symbol_index * 24U + 6U) == 0U,
                            "GOT relocation must point to undefined dynsym entry");
            }
            const std::size_t data_ph = 64U + 2U * 56U;
            n27_require(n27_u64(result.image.bytes, data_ph + 32U) == got_offset + import_count * 8U,
                        "RW p_filesz must include padding and initialized GOT slots");
            n27_require(n27_u64(result.image.bytes, data_ph + 40U) == got_offset + import_count * 8U + spec.bss_size,
                        "RW p_memsz must include GOT and BSS");
        }, passed, failed);
    }

    // N27C: patch x86-64 RIP-relative disp32 fields to the selected GOT slots.
    for (int index = 0; index < 500; ++index) {
        n27_case("N27C" + std::to_string(index + 1) + "_rip_relative_got_patch", [index] {
            auto spec = n27_basic_spec();
            spec.needed_libraries = {"libc.so.6"};
            const std::size_t import_count = static_cast<std::size_t>(1 + (index % 5));
            for (std::size_t i = 0U; i < import_count; ++i)
                spec.imports.push_back(Elf64ImportedSymbol{"api_" + std::to_string(i), Elf64DynamicSymbolKind::Function});
            const std::size_t reference_count = static_cast<std::size_t>(1 + (index % 4));
            spec.text.clear();
            for (std::size_t i = 0U; i < reference_count; ++i) {
                spec.text.insert(spec.text.end(), {0xffU, 0x15U, 0U, 0U, 0U, 0U, 0xc3U});
                spec.got_pc_relative32.push_back(Elf64GotPcRelative32{
                    i * 7U + 2U, "api_" + std::to_string((i + static_cast<std::size_t>(index)) % import_count)});
            }
            spec.exports.clear();
            spec.exports.push_back(Elf64ExportedSymbol{
                "x_n27_calls", Elf64SharedSection::Text, 0U, spec.text.size(), Elf64DynamicSymbolKind::Function});
            const auto result = write_elf64_shared_object(spec);
            n27_require(result.succeeded(), "valid RIP-relative GOT references should be patched");
            const auto rela = static_cast<std::size_t>(n27_dynamic_value(result.image, 7));
            (void)rela;
            const auto text = static_cast<std::size_t>(result.image.text_address);
            for (const auto& reference : spec.got_pc_relative32) {
                std::size_t sorted_import_index = 0U;
                for (const auto& imported : spec.imports) if (imported.name < reference.symbol_name) ++sorted_import_index;
                const std::uint64_t got_slot = result.image.got_address + sorted_import_index * 8U;
                const std::uint64_t place = result.image.text_address + reference.text_displacement_offset;
                const std::int64_t expected = static_cast<std::int64_t>(got_slot) - static_cast<std::int64_t>(place + 4U);
                n27_require(n27_i32(result.image.bytes, text + reference.text_displacement_offset) == expected,
                            "patched disp32 must equal GOT - (P + 4)");
            }
        }, passed, failed);
    }

    // N27D: fail-closed validation for dependencies, imports, GOT references and ranges.
    for (int index = 0; index < 500; ++index) {
        n27_case("N27D" + std::to_string(index + 1) + "_dynamic_import_rejection_matrix", [index] {
            auto spec = n27_basic_spec();
            spec.needed_libraries = {"libc.so.6"};
            spec.imports = {Elf64ImportedSymbol{"getpid", Elf64DynamicSymbolKind::Function}};
            spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U, 0xc3U};
            spec.got_pc_relative32 = {Elf64GotPcRelative32{2U, "getpid"}};
            switch (index % 12) {
            case 0: spec.needed_libraries.push_back("libc.so.6"); n27_expect_failure(spec, "XESHO0014"); break;
            case 1: spec.needed_libraries = {"/usr/lib/libc.so.6"}; n27_expect_failure(spec, "XESHO0014"); break;
            case 2: spec.needed_libraries.clear(); n27_expect_failure(spec, "XESHO0015"); break;
            case 3: spec.imports.push_back(spec.imports.front()); n27_expect_failure(spec, "XESHO0016"); break;
            case 4: spec.imports.front().name = spec.exports.front().name; n27_expect_failure(spec, "XESHO0016"); break;
            case 5: spec.imports.front().name = std::string("bad\0name", 8U); n27_expect_failure(spec, "XESHO0016"); break;
            case 6: spec.imports.front().kind = static_cast<Elf64DynamicSymbolKind>(99U); n27_expect_failure(spec, "XESHO0017"); break;
            case 7: spec.got_pc_relative32.front().symbol_name = "missing"; n27_expect_failure(spec, "XESHO0019"); break;
            case 8: spec.got_pc_relative32.front().text_displacement_offset = spec.text.size() - 2U; n27_expect_failure(spec, "XESHO0020"); break;
            case 9: spec.got_pc_relative32 = {Elf64GotPcRelative32{1U, "getpid"}, Elf64GotPcRelative32{3U, "getpid"}};
                    n27_expect_failure(spec, "XESHO0021"); break;
            case 10: spec.data.assign(8U, 0U); spec.bss_size = 32U;
                     spec.relative_relocations = {Elf64RelativeRelocation{9U, Elf64SharedSection::Text, 0U}};
                     n27_expect_failure(spec, "XESHO0012"); break;
            default: spec.needed_libraries = {std::string("bad\0soname", 10U)};
                     n27_expect_failure(spec, "XESHO0014"); break;
            }
        }, passed, failed);
    }
}
