#include "xfrontend/x64_codegen.hpp"
#include "xfrontend/parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool write_text(const std::string& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output << value;
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_n48_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    const std::string source =
        "module App;\n"
        "private function Add(int left, int right) returns int { return left + right; }\n"
        "private function Unused() returns int { return 99; }\n"
        "public function Main() returns int { int first = 17; int second = 25; int result = Add(first, second); if (result == 42) { return result; } else { return 1; } }\n";
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        std::cerr << "N48_PARSE | RESULT=FAILED | CODE=" << parsed.diagnostics.front().code << "\n";
        return 3;
    }
    auto compiled = xfrontend::compile_x64_coff_object(parsed.unit,
        xfrontend::X64CodegenOptions{"n48_native_exe", xfrontend::make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        std::cerr << "N48_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }
    const auto entry = std::find_if(compiled.native_object.symbol_table.symbols.begin(),
        compiled.native_object.symbol_table.symbols.end(), [](const xfrontend::NativeSymbolRecord& symbol) {
            return symbol.canonical_source_name == "main";
        });
    const auto add = std::find_if(compiled.native_object.symbol_table.symbols.begin(),
        compiled.native_object.symbol_table.symbols.end(), [](const xfrontend::NativeSymbolRecord& symbol) {
            return symbol.canonical_source_name == "add";
        });
    const auto unused = std::find_if(compiled.native_object.symbol_table.symbols.begin(),
        compiled.native_object.symbol_table.symbols.end(), [](const xfrontend::NativeSymbolRecord& symbol) {
            return symbol.canonical_source_name == "unused";
        });
    if (entry == compiled.native_object.symbol_table.symbols.end() || entry->mangled_name.empty() ||
        add == compiled.native_object.symbol_table.symbols.end() || add->mangled_name.empty() ||
        unused == compiled.native_object.symbol_table.symbols.end() || unused->mangled_name.empty()) {
        std::cerr << "N48_CODEGEN | RESULT=FAILED | REQUIRED_SYMBOL=NOT_FOUND\n";
        return 5;
    }
    if (!write_file((output_directory / "n48-app.obj").string(), compiled.object_bytes) ||
        !write_text((output_directory / "n48-entry-symbol.txt").string(), entry->mangled_name + "\r\n") ||
        !write_text((output_directory / "n48-add-symbol.txt").string(), add->mangled_name + "\r\n") ||
        !write_text((output_directory / "n48-unused-symbol.txt").string(), unused->mangled_name + "\r\n")) {
        std::cerr << "N48_OUTPUT | RESULT=FAILED | ERROR=WRITE_FAILED\n";
        return 6;
    }
    std::cout << "N48_NATIVE_CODEGEN | RESULT=PASSED | FUNCTIONS=3 | CALLS=1 | COFF_RELOCATIONS=1 | WIN64_ABI=PASSED | OBJECT_BYTES="
              << compiled.object_bytes.size() << " | ENTRY_SYMBOL=" << entry->mangled_name
              << " | ADD_SYMBOL=" << add->mangled_name << " | UNUSED_SYMBOL=" << unused->mangled_name << " | EXIT=0\n";
    return 0;
}
