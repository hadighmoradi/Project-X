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
        std::cerr << "usage: emit_n106_stack_probe_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) return 3;

    std::string source = "module N106StackProbeSmoke; public function Main() returns int { ";
    for (std::size_t index = 0U; index < 1024U; ++index) {
        source += "int stack_slot_" + std::to_string(index) + "=" + std::to_string(index) + "; ";
    }
    source += "return 42; }";

    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        const auto& diagnostic = parsed.diagnostics.front();
        std::cerr << "N106_STACK_PROBE_PARSE | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }
    const auto compiled = xfrontend::compile_x64_coff_object(parsed.unit,
        xfrontend::X64CodegenOptions{"n106-stack-probe-native-exe",
                                     xfrontend::make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        std::cerr << "N106_STACK_PROBE_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 5;
    }

    const auto* entry = find_symbol(compiled.native_object.symbol_table, "main");
    const auto helper = std::find_if(compiled.native_object.object_file.symbols.begin(),
        compiled.native_object.object_file.symbols.end(), [](const xfrontend::ObjectSymbol& symbol) {
            return symbol.name == "__chkstk" && !symbol.defined;
        });
    const auto probe_call = std::find_if(compiled.native_object.object_file.relocations.begin(),
        compiled.native_object.object_file.relocations.end(), [](const xfrontend::ObjectRelocation& relocation) {
            return relocation.target_symbol == "__chkstk" &&
                   relocation.kind == xfrontend::ObjectRelocationKind::CoffRelative32;
        });
    std::size_t pdata = 0U;
    std::size_t xdata = 0U;
    for (const auto& section : compiled.native_object.object_file.sections) {
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata;
    }
    if (entry == nullptr || entry->mangled_name.empty() || helper == compiled.native_object.object_file.symbols.end() ||
        probe_call == compiled.native_object.object_file.relocations.end() ||
        compiled.stats.stack_frame_bytes < 4096U || compiled.stats.win64_stack_probe_functions != 1U ||
        pdata != 1U || xdata != 1U) {
        std::cerr << "N106_STACK_PROBE_METADATA | RESULT=FAILED | FRAME=" << compiled.stats.stack_frame_bytes
                  << " | PROBES=" << compiled.stats.win64_stack_probe_functions
                  << " | PDATA=" << pdata << " | XDATA=" << xdata << "\n";
        return 6;
    }

    if (!write_bytes(output_directory / "n106-stack-probe.obj", compiled.object_bytes) ||
        !write_text(output_directory / "n106-stack-probe-entry-symbol.txt", entry->mangled_name + "\r\n") ||
        !write_text(output_directory / "n106-stack-probe-frame-size.txt",
                    std::to_string(compiled.stats.stack_frame_bytes) + "\r\n")) {
        std::cerr << "N106_STACK_PROBE_OUTPUT | RESULT=FAILED | WRITE_FAILED\n";
        return 7;
    }

    std::cout << "N106_STACK_PROBE_NATIVE_CODEGEN | RESULT=PASSED"
              << " | FRAME_BYTES=" << compiled.stats.stack_frame_bytes
              << " | PROBE_CALLS=1 | HELPER_SYMBOL=__chkstk"
              << " | PDATA=" << pdata << " | XDATA=" << xdata
              << " | OBJECT_BYTES=" << compiled.object_bytes.size()
              << " | ENTRY_SYMBOL=" << entry->mangled_name << " | EXIT=0\n";
    return 0;
}
