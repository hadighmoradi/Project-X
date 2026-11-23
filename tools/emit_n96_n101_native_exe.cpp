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
        std::cerr << "usage: emit_n96_n101_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) return 3;

    // Eight long-lived scalar locals create real GP pressure. e/f/g/h remain
    // live across Helper; nonvolatile registers preserve them across the call,
    // while overflow values retain canonical stack homes. The result is 42.
    const std::string source =
        "module N96N101Smoke;\n"
        "private function Helper(int input) returns int { return input + 3; }\n"
        "private function Unused() returns int { return 99; }\n"
        "public function Main() returns int {\n"
        "  int a = 1; int b = 2; int c = 3; int d = 4;\n"
        "  int e = 5; int f = 6; int g = 7; int h = 11;\n"
        "  double fa = 1.5; double fb = 2.5; double fc = 3.5; double fd = 4.5;\n"
        "  double total = fa + fb + fc + fd;\n"
        "  int result = Helper(a + b + c + d);\n"
        "  return result + e + f + g + h;\n"
        "}\n";

    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        const auto& diagnostic = parsed.diagnostics.front();
        std::cerr << "N96_N101_PARSE | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }
    const auto compiled = xfrontend::compile_x64_coff_object(parsed.unit,
        xfrontend::X64CodegenOptions{"n96-n101-persistent-register-native-exe",
                                     xfrontend::make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        std::cerr << "N96_N101_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 5;
    }

    const auto* entry = find_symbol(compiled.native_object.symbol_table, "main");
    const auto* helper = find_symbol(compiled.native_object.symbol_table, "helper");
    const auto* unused = find_symbol(compiled.native_object.symbol_table, "unused");
    if (entry == nullptr || helper == nullptr || unused == nullptr ||
        entry->mangled_name.empty() || helper->mangled_name.empty() || unused->mangled_name.empty()) {
        std::cerr << "N96_N101_SYMBOLS | RESULT=FAILED | SYMBOL_TABLE_INCOMPLETE\n";
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
        compiled.stats.native_persistent_gp_register_assignments == 0U ||
        compiled.stats.native_persistent_xmm_register_assignments == 0U ||
        compiled.stats.callee_saved_gp_registers == 0U ||
        compiled.stats.callee_saved_xmm_registers == 0U ||
        compiled.stats.native_persistent_register_reads == 0U ||
        compiled.stats.native_persistent_register_writes == 0U ||
        compiled.stats.native_persistent_register_stack_fallbacks == 0U) {
        std::cerr << "N96_N101_METADATA | RESULT=FAILED"
                  << " | PDATA=" << pdata_count << " | XDATA=" << xdata_count
                  << " | ADDR32NB=" << addr32nb_count
                  << " | GP_REGS=" << compiled.stats.native_persistent_gp_register_assignments
                  << " | XMM_REGS=" << compiled.stats.native_persistent_xmm_register_assignments
                  << " | CALLEE_GP=" << compiled.stats.callee_saved_gp_registers
                  << " | CALLEE_XMM=" << compiled.stats.callee_saved_xmm_registers
                  << " | GP_READS=" << compiled.stats.native_persistent_register_reads
                  << " | GP_WRITES=" << compiled.stats.native_persistent_register_writes
                  << " | STACK_FALLBACKS=" << compiled.stats.native_persistent_register_stack_fallbacks << "\n";
        return 7;
    }

    if (!write_bytes(output_directory / "n96-n101-persistent-registers.obj", compiled.object_bytes) ||
        !write_text(output_directory / "n96-n101-entry-symbol.txt", entry->mangled_name + "\r\n") ||
        !write_text(output_directory / "n96-n101-helper-symbol.txt", helper->mangled_name + "\r\n") ||
        !write_text(output_directory / "n96-n101-unused-symbol.txt", unused->mangled_name + "\r\n")) {
        std::cerr << "N96_N101_OUTPUT | RESULT=FAILED | WRITE_FAILED\n";
        return 8;
    }

    std::cout << "N96_N101_NATIVE_CODEGEN | RESULT=PASSED | FUNCTIONS=3"
              << " | GP_REGS=" << compiled.stats.native_persistent_gp_register_assignments
              << " | XMM_REGS=" << compiled.stats.native_persistent_xmm_register_assignments
              << " | CALLEE_GP=" << compiled.stats.callee_saved_gp_registers
              << " | CALLEE_XMM=" << compiled.stats.callee_saved_xmm_registers
              << " | REGISTER_READS=" << compiled.stats.native_persistent_register_reads
              << " | REGISTER_WRITES=" << compiled.stats.native_persistent_register_writes
              << " | STACK_FALLBACKS=" << compiled.stats.native_persistent_register_stack_fallbacks
              << " | ADDRESS_TAKEN_MEMORY=" << compiled.stats.native_address_taken_variables_kept_in_memory
              << " | PDATA=" << pdata_count << " | XDATA=" << xdata_count
              << " | ADDR32NB=" << addr32nb_count
              << " | OBJECT_BYTES=" << compiled.object_bytes.size()
              << " | ENTRY_SYMBOL=" << entry->mangled_name
              << " | HELPER_SYMBOL=" << helper->mangled_name
              << " | UNUSED_SYMBOL=" << unused->mangled_name << " | EXIT=0\n";
    return 0;
}
