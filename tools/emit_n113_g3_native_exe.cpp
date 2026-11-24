// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/parser.hpp"
#include "xfrontend/x64_codegen.hpp"

#include <algorithm>
#include <cstdint>
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
        std::cerr << "usage: emit_n113_g3_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) return 3;

    // This is the same multi-function, call, branch, loop and GP/XMM pressure
    // fixture used by N113-G2's real FunctionEmitter insertion test.
    const std::string source =
        "module N113Emitter; "
        "private function Helper(int p, int q) returns int { return p + q; } "
        "private function FloatHelper(double x, double y) returns double { return x + y; } "
        "private function Unused() returns int { return 77; } "
        "public function Main() returns int { "
        "int a=1; int b=2; int c=3; int d=4; int e=5; int f=6; "
        "double x=1.0; double y=2.0; double z=3.0; double w=4.0; "
        "double total = FloatHelper(x+y, z+w); "
        "int i=0; int result=0; "
        "while (i < 4) { if (i < 2) { result = result + Helper(a,b); } "
        "else { result = result + Helper(c,d); } i = i + 1; } "
        "return result + e + f; }";

    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        const auto& diagnostic = parsed.diagnostics.front();
        std::cerr << "N113_G3_PARSE | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }

    xfrontend::X64CodegenOptions options{
        "n113-g3-g2-e2e-native-exe", xfrontend::make_coff_x86_64_object_target()};
    options.insert_n113_emitter_spill_reload_actions = true;
    const auto compiled = xfrontend::compile_x64_coff_object(parsed.unit, options);
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        std::cerr << "N113_G3_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 5;
    }

    const auto* entry = find_symbol(compiled.native_object.symbol_table, "main");
    const auto* helper = find_symbol(compiled.native_object.symbol_table, "helper");
    if (entry == nullptr || entry->mangled_name.empty() || helper == nullptr || helper->mangled_name.empty()) {
        std::cerr << "N113_G3_SYMBOLS | RESULT=FAILED | ENTRY_OR_HELPER_MISSING\n";
        return 6;
    }

    std::size_t pdata = 0U;
    std::size_t xdata = 0U;
    for (const auto& section : compiled.native_object.object_file.sections) {
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata;
    }
    if (pdata == 0U || pdata != xdata ||
        compiled.stats.win64_unwind_records_emitted != pdata) {
        std::cerr << "N113_G3_UNWIND | RESULT=FAILED | PDATA=" << pdata
                  << " | XDATA=" << xdata
                  << " | UNWIND_STATS=" << compiled.stats.win64_unwind_records_emitted << "\n";
        return 7;
    }
    if (compiled.stats.native_n113_emitter_actions_inserted == 0U ||
        compiled.stats.native_n113_emitter_actions_inserted !=
            compiled.stats.native_n113_emitter_actions_rebound ||
        compiled.stats.native_n113_emitter_machine_moves_inserted <
            compiled.stats.native_n113_emitter_actions_inserted ||
        compiled.stats.native_n113_emitter_transfer_bytes_inserted == 0U) {
        std::cerr << "N113_G3_TRANSFER_STATS | RESULT=FAILED"
                  << " | REBOUND=" << compiled.stats.native_n113_emitter_actions_rebound
                  << " | INSERTED=" << compiled.stats.native_n113_emitter_actions_inserted
                  << " | MOVES=" << compiled.stats.native_n113_emitter_machine_moves_inserted
                  << " | BYTES=" << compiled.stats.native_n113_emitter_transfer_bytes_inserted << "\n";
        return 8;
    }
    if (compiled.native_object.object_file.relocations.empty()) {
        std::cerr << "N113_G3_RELOCATIONS | RESULT=FAILED | NO_RELOCATIONS\n";
        return 9;
    }
    for (const auto& relocation : compiled.native_object.object_file.relocations) {
        const auto section = std::find_if(compiled.native_object.object_file.sections.begin(),
            compiled.native_object.object_file.sections.end(), [&](const xfrontend::ObjectSection& item) {
                return item.id == relocation.section_id;
            });
        if (section == compiled.native_object.object_file.sections.end() ||
            relocation.offset > section->size || relocation.width > section->size - relocation.offset) {
            std::cerr << "N113_G3_RELOCATIONS | RESULT=FAILED | OUT_OF_RANGE_RELOCATION\n";
            return 10;
        }
    }

    constexpr int expected_exit = 31;
    const std::string summary =
        "N113_G3_NATIVE_CODEGEN | RESULT=PASSED"
        " | G2_ACTIONS=" + std::to_string(compiled.stats.native_n113_emitter_actions_inserted) +
        " | MACHINE_MOVES=" + std::to_string(compiled.stats.native_n113_emitter_machine_moves_inserted) +
        " | TRANSFER_BYTES=" + std::to_string(compiled.stats.native_n113_emitter_transfer_bytes_inserted) +
        " | PDATA=" + std::to_string(pdata) + " | XDATA=" + std::to_string(xdata) +
        " | RELOCATIONS=" + std::to_string(compiled.native_object.object_file.relocations.size()) +
        " | OBJECT_BYTES=" + std::to_string(compiled.object_bytes.size()) +
        " | ENTRY_SYMBOL=" + entry->mangled_name +
        " | HELPER_SYMBOL=" + helper->mangled_name +
        " | EXPECTED_EXIT=" + std::to_string(expected_exit) + " | EXIT=0";
    if (!write_bytes(output_directory / "n113-g3-g2-inserted.obj", compiled.object_bytes) ||
        !write_text(output_directory / "n113-g3-entry-symbol.txt", entry->mangled_name + "\r\n") ||
        !write_text(output_directory / "n113-g3-helper-symbol.txt", helper->mangled_name + "\r\n") ||
        !write_text(output_directory / "n113-g3-expected-exit.txt", std::to_string(expected_exit) + "\r\n") ||
        !write_text(output_directory / "n113-g3-codegen-summary.txt", summary + "\r\n")) {
        std::cerr << "N113_G3_OUTPUT | RESULT=FAILED | WRITE_FAILED\n";
        return 11;
    }

    std::cout << summary << "\n";
    return 0;
}
