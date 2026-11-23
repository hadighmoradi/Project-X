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
        std::cerr << "usage: emit_n102_n107_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) return 3;

    const std::string source =
        "module N102N107Smoke; "
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
        "return result + e + f + 11; }";

    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        const auto& diagnostic = parsed.diagnostics.front();
        std::cerr << "N102_N107_PARSE | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }
    const auto compiled = xfrontend::compile_x64_coff_object(parsed.unit,
        xfrontend::X64CodegenOptions{"n102-n107-stress-native-exe",
                                     xfrontend::make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        std::cerr << "N102_N107_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 5;
    }

    const auto* entry = find_symbol(compiled.native_object.symbol_table, "main");
    const auto* helper = find_symbol(compiled.native_object.symbol_table, "helper");
    const auto* unused = find_symbol(compiled.native_object.symbol_table, "unused");
    if (entry == nullptr || helper == nullptr || unused == nullptr ||
        entry->mangled_name.empty() || helper->mangled_name.empty() || unused->mangled_name.empty()) {
        std::cerr << "N102_N107_SYMBOLS | RESULT=FAILED | SYMBOL_TABLE_INCOMPLETE\n";
        return 6;
    }

    std::size_t pdata = 0U;
    std::size_t xdata = 0U;
    std::size_t addr32nb = 0U;
    for (const auto& section : compiled.native_object.object_file.sections) {
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata;
    }
    for (const auto& relocation : compiled.native_object.object_file.relocations)
        if (relocation.kind == xfrontend::ObjectRelocationKind::CoffAddr32NB) ++addr32nb;
    if (pdata != 4U || xdata != 4U || addr32nb != 12U ||
        compiled.stats.native_persistent_gp_register_assignments == 0U ||
        compiled.stats.native_persistent_xmm_register_assignments == 0U ||
        compiled.stats.native_persistent_home_mirror_stores_elided == 0U) {
        std::cerr << "N102_N107_METADATA | RESULT=FAILED | PDATA=" << pdata
                  << " | XDATA=" << xdata << " | ADDR32NB=" << addr32nb
                  << " | GP=" << compiled.stats.native_persistent_gp_register_assignments
                  << " | XMM=" << compiled.stats.native_persistent_xmm_register_assignments
                  << " | MIRROR_STORES_ELIDED=" << compiled.stats.native_persistent_home_mirror_stores_elided << "\n";
        return 7;
    }

    if (!write_bytes(output_directory / "n102-n107-stress.obj", compiled.object_bytes) ||
        !write_text(output_directory / "n102-n107-entry-symbol.txt", entry->mangled_name + "\r\n") ||
        !write_text(output_directory / "n102-n107-helper-symbol.txt", helper->mangled_name + "\r\n") ||
        !write_text(output_directory / "n102-n107-unused-symbol.txt", unused->mangled_name + "\r\n")) {
        std::cerr << "N102_N107_OUTPUT | RESULT=FAILED | WRITE_FAILED\n";
        return 8;
    }

    std::cout << "N102_N107_NATIVE_CODEGEN | RESULT=PASSED | FUNCTIONS=4"
              << " | GP_REGS=" << compiled.stats.native_persistent_gp_register_assignments
              << " | XMM_REGS=" << compiled.stats.native_persistent_xmm_register_assignments
              << " | HOME_STORES_ELIDED=" << compiled.stats.native_persistent_home_mirror_stores_elided
              << " | PDATA=" << pdata << " | XDATA=" << xdata
              << " | ADDR32NB=" << addr32nb << " | OBJECT_BYTES=" << compiled.object_bytes.size()
              << " | ENTRY_SYMBOL=" << entry->mangled_name
              << " | HELPER_SYMBOL=" << helper->mangled_name
              << " | UNUSED_SYMBOL=" << unused->mangled_name << " | EXIT=0\n";
    return 0;
}
