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
        std::cerr << "usage: emit_n54_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) return 3;
    const std::string source =
        "module App;\n"
        "private function Add(int left, int right) returns int { return left + right; }\n"
        "private function Unused() returns int { return 99; }\n"
        "public function Main() returns int { int seed = 17; return (seed + 8) + Add(7, 10); }\n";
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        std::cerr << "N54_PARSE | RESULT=FAILED | CODE=" << parsed.diagnostics.front().code << "\n";
        return 4;
    }
    const auto compiled = xfrontend::compile_x64_coff_object(parsed.unit,
        xfrontend::X64CodegenOptions{"n54-native-register-exe", xfrontend::make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        std::cerr << "N54_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code << " | MESSAGE=" << diagnostic.message << "\n";
        return 5;
    }
    const auto* main_symbol = find_symbol(compiled.native_object.symbol_table, "main");
    const auto* add_symbol = find_symbol(compiled.native_object.symbol_table, "add");
    const auto* unused_symbol = find_symbol(compiled.native_object.symbol_table, "unused");
    if (main_symbol == nullptr || add_symbol == nullptr || unused_symbol == nullptr ||
        main_symbol->mangled_name.empty() || add_symbol->mangled_name.empty() || unused_symbol->mangled_name.empty()) {
        std::cerr << "N54_CODEGEN | RESULT=FAILED | SYMBOL_TABLE_INCOMPLETE\n";
        return 6;
    }
    if (!write_bytes(output_directory / "n54-app.obj", compiled.object_bytes) ||
        !write_text(output_directory / "n54-entry-symbol.txt", main_symbol->mangled_name + "\r\n") ||
        !write_text(output_directory / "n54-add-symbol.txt", add_symbol->mangled_name + "\r\n") ||
        !write_text(output_directory / "n54-unused-symbol.txt", unused_symbol->mangled_name + "\r\n")) {
        std::cerr << "N54_OUTPUT | RESULT=FAILED | WRITE_FAILED\n";
        return 7;
    }
    std::cout << "N54_NATIVE_CODEGEN | RESULT=PASSED | FUNCTIONS=3 | INTEGER_TEMP_REGISTERS="
              << compiled.stats.volatile_register_temporaries
              << " | STACK_TEMP_SPILLS=" << compiled.stats.stack_spilled_temporaries
              << " | STACK_TEMP_BYTES_AVOIDED=" << compiled.stats.stack_temporary_bytes_avoided
              << " | COFF_RELOCATIONS=" << compiled.native_object.object_file.relocations.size()
              << " | OBJECT_BYTES=" << compiled.object_bytes.size()
              << " | ENTRY_SYMBOL=" << main_symbol->mangled_name
              << " | ADD_SYMBOL=" << add_symbol->mangled_name
              << " | UNUSED_SYMBOL=" << unused_symbol->mangled_name << " | EXIT=0\n";
    if (compiled.stats.volatile_register_temporaries < 2U || compiled.stats.stack_spilled_temporaries < 1U) {
        std::cerr << "N54_NATIVE_CODEGEN | RESULT=FAILED | EXPECTED_REGISTER_AND_CALL_SPILL_PATHS_NOT_USED\n";
        return 8;
    }
    return 0;
}
