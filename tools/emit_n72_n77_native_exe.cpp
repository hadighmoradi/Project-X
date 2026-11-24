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
using namespace xfrontend;

std::string fold_name(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

std::string make_right_deep_sum(const std::string& variable, std::size_t leaves) {
    std::string expression = variable;
    for (std::size_t index = 1U; index < leaves; ++index)
        expression = variable + " + (" + expression + ")";
    return expression;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << value << "\r\n";
    return static_cast<bool>(output);
}

const NativeSymbolRecord* find_symbol(const X64CodegenResult& result, const std::string& name) {
    const auto found = std::find_if(result.native_object.symbol_table.symbols.begin(),
        result.native_object.symbol_table.symbols.end(), [&](const NativeSymbolRecord& symbol) {
            return fold_name(symbol.canonical_source_name) == fold_name(name);
        });
    return found == result.native_object.symbol_table.symbols.end() ? nullptr : &*found;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_n72_n77_native_exe <output-directory>\n";
        return 2;
    }

    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) {
        std::cerr << "cannot create output directory: " << error.message() << '\n';
        return 3;
    }

    const std::string gp_pressure = make_right_deep_sum("seed", 22U);
    const std::string xmm_pressure = make_right_deep_sum("floatingSeed", 22U);
    const std::string source =
        "module N72N77Smoke;\n"
        "private function Helper(int value) returns int { return value + 0; }\n"
        "private function Unused() returns int { return 99; }\n"
        "public function Main() returns int {\n"
        "  int seed = 0;\n"
        "  double floatingSeed = 0.0;\n"
        "  int gpPressure = " + gp_pressure + ";\n"
        "  double xmmPressure = " + xmm_pressure + ";\n"
        "  int callPressure = seed + (seed + (seed + Helper(seed)));\n"
        "  if (gpPressure == 0 && xmmPressure == 0.0 && callPressure == 0) { return 42; }\n"
        "  else { return 1; }\n"
        "}\n";

    auto parsed = parse_source(source);
    if (!parsed.succeeded()) {
        for (const auto& diagnostic : parsed.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 4;
    }

    const auto result = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n72-n77-register-codegen-smoke", make_coff_x86_64_object_target()});
    if (!result.succeeded()) {
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 5;
    }

    const auto* entry = find_symbol(result, "Main");
    const auto* helper = find_symbol(result, "Helper");
    const auto* unused = find_symbol(result, "Unused");
    if (entry == nullptr || helper == nullptr || unused == nullptr ||
        entry->mangled_name.empty() || helper->mangled_name.empty() || unused->mangled_name.empty()) {
        std::cerr << "required COFF symbols were not emitted\n";
        return 6;
    }
    if (result.stats.native_register_values_analyzed < 40U ||
        result.stats.native_spill_slots == 0U ||
        result.stats.callee_saved_gp_registers == 0U ||
        result.stats.callee_saved_xmm_registers == 0U ||
        (result.stats.stack_frame_bytes % 16U) != 0U ||
        result.native_object.object_file.relocations.empty()) {
        std::cerr << "register-pressure smoke did not exercise expected allocation/frame/relocation paths"
                  << " | VALUES=" << result.stats.native_register_values_analyzed
                  << " | SPILL_SLOTS=" << result.stats.native_spill_slots
                  << " | CALLEE_GP=" << result.stats.callee_saved_gp_registers
                  << " | CALLEE_XMM=" << result.stats.callee_saved_xmm_registers
                  << " | FRAME_BYTES=" << result.stats.stack_frame_bytes
                  << " | RELOCS=" << result.native_object.object_file.relocations.size() << '\n';
        return 7;
    }

    if (!write_bytes(output_directory / "n72-n77-register-pressure.obj", result.object_bytes) ||
        !write_text(output_directory / "n72-n77-entry-symbol.txt", entry->mangled_name) ||
        !write_text(output_directory / "n72-n77-helper-symbol.txt", helper->mangled_name) ||
        !write_text(output_directory / "n72-n77-unused-symbol.txt", unused->mangled_name)) {
        std::cerr << "failed writing N72-N77 COFF smoke artifacts\n";
        return 8;
    }

    std::cout << "N72_N77_SOURCE_EMIT | RESULT=PASSED"
              << " | VALUES=" << result.stats.native_register_values_analyzed
              << " | SPILL_SLOTS=" << result.stats.native_spill_slots
              << " | CALLEE_GP=" << result.stats.callee_saved_gp_registers
              << " | CALLEE_XMM=" << result.stats.callee_saved_xmm_registers
              << " | FRAME_BYTES=" << result.stats.stack_frame_bytes
              << " | RELOCATIONS=" << result.native_object.object_file.relocations.size()
              << " | OBJECT_BYTES=" << result.object_bytes.size()
              << " | EXIT=0\n";
    return 0;
}
