#include "xfrontend/native_program_pipeline.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/x64_codegen.hpp"

#include <algorithm>
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

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << text << "\r\n";
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
        std::cerr << "usage: emit_n55_n59_native_exe <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) return 3;

    const std::string math_source =
        "module Math;\n"
        "private function UnusedMath() returns int { return 97; }\n"
        "public function Add(double left, double right) returns double { return left + right; }\n";
    const std::string app_source =
        "module App;\n"
        "import Math;\n"
        "private function Bump(int value) returns int { return value + 1; }\n"
        "private function UnusedApp() returns int { return 99; }\n"
        "public function Main() returns int {\n"
        "  double total = Math.Add(17.5, 24.5);\n"
        "  int count = 17;\n"
        "  int folded = 6 + 7;\n"
        "  int* pointer = &count;\n"
        "  *pointer = Bump(*pointer + 24);\n"
        "  { int temporary = 10; }\n"
        "  { double temporaryFloat = 2.5; }\n"
        "  if (total == 42.0 && *pointer == 42 && folded == 13) { return 42; } else { return 1; }\n"
        "}\n";

    auto math_parsed = parse_source(math_source);
    auto app_parsed = parse_source(app_source);
    if (!math_parsed.succeeded() || !app_parsed.succeeded()) {
        const auto& diagnostic = !math_parsed.succeeded() ? math_parsed.diagnostics.front() : app_parsed.diagnostics.front();
        std::cerr << "N55_N59_PARSE | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }

    const auto program = compile_x64_coff_program({
        NativeProgramModuleInput{"Math.src", std::make_shared<CompilationUnit>(std::move(math_parsed.unit))},
        NativeProgramModuleInput{"App.src", std::make_shared<CompilationUnit>(std::move(app_parsed.unit))}});
    if (!program.succeeded() || program.modules.size() != 2U) {
        const std::string message = program.diagnostics.empty() ? "multi-module compilation did not return two modules" :
            program.diagnostics.front().code + ": " + program.diagnostics.front().message;
        std::cerr << "N55_N59_PIPELINE | RESULT=FAILED | MESSAGE=" << message << "\n";
        return 5;
    }

    const auto app_module = std::find_if(program.modules.begin(), program.modules.end(), [](const auto& module) {
        return fold_name(module.module_name) == "app";
    });
    const auto math_module = std::find_if(program.modules.begin(), program.modules.end(), [](const auto& module) {
        return fold_name(module.module_name) == "math";
    });
    if (app_module == program.modules.end() || math_module == program.modules.end()) return 6;
    const auto* main_symbol = find_symbol(app_module->codegen, "main");
    const auto* bump_symbol = find_symbol(app_module->codegen, "bump");
    const auto* unused_app_symbol = find_symbol(app_module->codegen, "unusedapp");
    const auto* add_symbol = find_symbol(math_module->codegen, "add");
    const auto* unused_math_symbol = find_symbol(math_module->codegen, "unusedmath");
    if (main_symbol == nullptr || bump_symbol == nullptr || unused_app_symbol == nullptr ||
        add_symbol == nullptr || unused_math_symbol == nullptr || main_symbol->mangled_name.empty() ||
        bump_symbol->mangled_name.empty() || add_symbol->mangled_name.empty()) return 7;

    if (math_module->codegen.stats.xmm_register_temporaries == 0U ||
        app_module->codegen.stats.local_stack_slots_reused == 0U ||
        app_module->codegen.native_object.object_file.relocations.empty()) {
        std::cerr << "N55_N59_PIPELINE | RESULT=FAILED | XMM_OR_STACK_REUSE_OR_RELOCATION_NOT_OBSERVED\n";
        return 8;
    }

    const std::string benchmark_source =
        "module Benchmark;\n"
        "public function Main() returns int {\n"
        "  int index = 0;\n"
        "  int total = 0;\n"
        "  while (index < 500000) { total = total + index; index = index + 1; }\n"
        "  if (total == 124999750000) { return 42; } else { return 1; }\n"
        "}\n";
    auto benchmark_parsed = parse_source(benchmark_source);
    if (!benchmark_parsed.succeeded()) {
        std::cerr << "N58_BENCHMARK_PARSE | RESULT=FAILED | CODE=" << benchmark_parsed.diagnostics.front().code << "\n";
        return 9;
    }
    const auto benchmark = compile_x64_coff_object(benchmark_parsed.unit,
        X64CodegenOptions{"n55-n59-benchmark", make_coff_x86_64_object_target()});
    if (!benchmark.succeeded()) {
        const auto& diagnostic = benchmark.diagnostics.front();
        std::cerr << "N58_BENCHMARK_CODEGEN | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 10;
    }
    const auto* benchmark_main = find_symbol(benchmark, "main");
    if (benchmark_main == nullptr || benchmark_main->mangled_name.empty()) return 11;

    if (!write_bytes(output_directory / "n55-n59-app.obj", app_module->codegen.object_bytes) ||
        !write_bytes(output_directory / "n55-n59-math.obj", math_module->codegen.object_bytes) ||
        !write_bytes(output_directory / "n55-n59-benchmark.obj", benchmark.object_bytes) ||
        !write_text(output_directory / "n55-n59-entry-symbol.txt", main_symbol->mangled_name) ||
        !write_text(output_directory / "n55-n59-add-symbol.txt", add_symbol->mangled_name) ||
        !write_text(output_directory / "n55-n59-bump-symbol.txt", bump_symbol->mangled_name) ||
        !write_text(output_directory / "n55-n59-unused-app-symbol.txt", unused_app_symbol->mangled_name) ||
        !write_text(output_directory / "n55-n59-unused-math-symbol.txt", unused_math_symbol->mangled_name) ||
        !write_text(output_directory / "n55-n59-benchmark-entry-symbol.txt", benchmark_main->mangled_name)) return 12;

    std::cout << "N55_N59_SOURCE_EMIT | RESULT=PASSED | MODULES=2 | OBJECTS=3"
              << " | XMM_TEMPORARIES=" << math_module->codegen.stats.xmm_register_temporaries
              << " | XMM_SPILLS=" << math_module->codegen.stats.xmm_stack_spilled_temporaries
              << " | LOCAL_SLOTS_REUSED=" << app_module->codegen.stats.local_stack_slots_reused
              << " | FOLDED_EXPRESSIONS=" << (app_module->codegen.stats.constant_folded_expressions + math_module->codegen.stats.constant_folded_expressions)
              << " | APP_RELOCATIONS=" << app_module->codegen.native_object.object_file.relocations.size()
              << " | APP_OBJECT_BYTES=" << app_module->codegen.object_bytes.size()
              << " | MATH_OBJECT_BYTES=" << math_module->codegen.object_bytes.size()
              << " | BENCH_OBJECT_BYTES=" << benchmark.object_bytes.size()
              << " | EXIT=0\n";
    std::cout << "N58_BENCHMARK_SOURCE | ITERATIONS=500000 | EXPECTED_SUM=124999750000 | ENTRY_SYMBOL="
              << benchmark_main->mangled_name << " | EXIT=0\n";
    return 0;
}
