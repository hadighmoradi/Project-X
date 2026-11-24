// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/parser.hpp"
#include "xfrontend/x64_codegen.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}
bool write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output << value;
    return static_cast<bool>(output);
}
const xfrontend::NativeSymbolRecord* find_symbol(
    const xfrontend::NativeSymbolTableResult& table, const std::string& source_name) {
    const auto found = std::find_if(table.symbols.begin(), table.symbols.end(), [&](const auto& symbol) {
        return symbol.canonical_source_name == source_name;
    });
    return found == table.symbols.end() ? nullptr : &*found;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_n90_n95_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) return 3;

    // Branch-specific stores feed canonical local homes; the loop back-edge
    // and join use the same homes, so memory-SSA Phis coalesce without copies.
    // Main is a no-argument native entry point and deterministically returns 42.
    const std::string source =
        "module N90N95Smoke;\n"
        "private function Helper(int input) returns int { return input + 4; }\n"
        "private function Unused() returns int { return 99; }\n"
        "public function Main() returns int {\n"
        "  int result = 0; int i = 0; int branch_value = 0;\n"
        "  while (i < 4) {\n"
        "    if (i == 1) { branch_value = Helper(i); result = result + branch_value; }\n"
        "    else { branch_value = i + 2; result = result + branch_value; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  if (result > 0) { return 42; } else { return 1; }\n"
        "}\n";

    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        const auto& diagnostic = parsed.diagnostics.front();
        std::cerr << "N90_N95_PARSE | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }
    const auto compiled = xfrontend::compile_x64_coff_object(parsed.unit,
        xfrontend::X64CodegenOptions{"n90-n95-machine-home-native-exe",
                                     xfrontend::make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        std::cerr << "N90_N95_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 5;
    }

    const auto* entry = find_symbol(compiled.native_object.symbol_table, "main");
    const auto* helper = find_symbol(compiled.native_object.symbol_table, "helper");
    const auto* unused = find_symbol(compiled.native_object.symbol_table, "unused");
    if (entry == nullptr || helper == nullptr || unused == nullptr ||
        entry->mangled_name.empty() || helper->mangled_name.empty() || unused->mangled_name.empty()) {
        std::cerr << "N90_N95_SYMBOLS | RESULT=FAILED | SYMBOL_TABLE_INCOMPLETE\n";
        return 6;
    }

    std::size_t pdata_count = 0U;
    std::size_t xdata_count = 0U;
    std::size_t addr32nb_count = 0U;
    for (const auto& section : compiled.native_object.object_file.sections) {
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata_count;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata_count;
    }
    for (const auto& relocation : compiled.native_object.object_file.relocations)
        if (relocation.kind == xfrontend::ObjectRelocationKind::CoffAddr32NB) ++addr32nb_count;
    if (pdata_count != 3U || xdata_count != 3U || addr32nb_count != 9U ||
        compiled.stats.win64_unwind_records_emitted != 3U ||
        compiled.stats.native_persistent_homes_mapped < 4U ||
        compiled.stats.native_home_phi_nodes == 0U ||
        compiled.stats.native_home_phi_edges_coalesced != compiled.stats.native_home_phi_incoming_edges ||
        compiled.stats.native_home_phi_machine_copies != 0U) {
        std::cerr << "N90_N95_METADATA | RESULT=FAILED"
                  << " | PDATA=" << pdata_count << " | XDATA=" << xdata_count
                  << " | ADDR32NB=" << addr32nb_count
                  << " | HOMES=" << compiled.stats.native_persistent_homes_mapped
                  << " | PHIS=" << compiled.stats.native_home_phi_nodes
                  << " | PHI_EDGES=" << compiled.stats.native_home_phi_incoming_edges
                  << " | COALESCED=" << compiled.stats.native_home_phi_edges_coalesced
                  << " | COPIES=" << compiled.stats.native_home_phi_machine_copies << "\n";
        return 7;
    }

    if (!write_bytes(output_directory / "n90-n95-multiblock.obj", compiled.object_bytes) ||
        !write_text(output_directory / "n90-n95-entry-symbol.txt", entry->mangled_name + "\r\n") ||
        !write_text(output_directory / "n90-n95-helper-symbol.txt", helper->mangled_name + "\r\n") ||
        !write_text(output_directory / "n90-n95-unused-symbol.txt", unused->mangled_name + "\r\n")) {
        std::cerr << "N90_N95_OUTPUT | RESULT=FAILED | WRITE_FAILED\n";
        return 8;
    }

    std::cout << "N90_N95_NATIVE_CODEGEN | RESULT=PASSED | FUNCTIONS=3"
              << " | PERSISTENT_HOMES=" << compiled.stats.native_persistent_homes_mapped
              << " | CALL_CROSSING_HOMES=" << compiled.stats.native_call_crossing_values_homed
              << " | MEMORY_PHIS=" << compiled.stats.native_home_phi_nodes
              << " | PHI_INCOMING_EDGES=" << compiled.stats.native_home_phi_incoming_edges
              << " | PHI_COALESCED=" << compiled.stats.native_home_phi_edges_coalesced
              << " | PHI_MACHINE_COPIES=" << compiled.stats.native_home_phi_machine_copies
              << " | UNWIND_RECORDS=" << compiled.stats.win64_unwind_records_emitted
              << " | PDATA=" << pdata_count << " | XDATA=" << xdata_count
              << " | ADDR32NB=" << addr32nb_count
              << " | OBJECT_BYTES=" << compiled.object_bytes.size()
              << " | ENTRY_SYMBOL=" << entry->mangled_name
              << " | HELPER_SYMBOL=" << helper->mangled_name
              << " | UNUSED_SYMBOL=" << unused->mangled_name << " | EXIT=0\n";
    return 0;
}
