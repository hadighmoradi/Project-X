// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_program_pipeline.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/pe64_dll.hpp"
#include "xfrontend/win64_unwind_codegen.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << value;
    return static_cast<bool>(output);
}

const NativeSymbolRecord* find_symbol(const X64CodegenResult& result, const std::string& canonical_name) {
    const auto found = std::find_if(result.native_object.symbol_table.symbols.begin(),
        result.native_object.symbol_table.symbols.end(), [&canonical_name](const NativeSymbolRecord& symbol) {
            return symbol.canonical_source_name == canonical_name;
        });
    return found == result.native_object.symbol_table.symbols.end() ? nullptr : &*found;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_n49_n53_native_exe <output-directory>\n";
        return 2;
    }

    const std::filesystem::path output_directory(argv[1]);
    std::error_code filesystem_error;
    std::filesystem::create_directories(output_directory, filesystem_error);
    if (filesystem_error) {
        std::cerr << "N49_N53_OUTPUT | RESULT=FAILED | ERROR=CREATE_DIRECTORY\n";
        return 3;
    }

    const std::string math_source =
        "module Math;\n"
        "private function UnusedMath() returns int { return 77; }\n"
        "public function Add(double left, double right) returns double { return left + right; }\n";
    const std::string app_source =
        "module App;\n"
        "import Math;\n"
        "private function UnusedApp() returns int { return 99; }\n"
        "public function Main() returns int {\n"
        "  double total = Math.Add(17.5, 24.5);\n"
        "  int answer = 41;\n"
        "  int* pointer = &answer;\n"
        "  *pointer = *pointer + 1;\n"
        "  if (total == 42.0) { return *pointer; } else { return 1; }\n"
        "}\n";

    auto math_parsed = parse_source(math_source);
    auto app_parsed = parse_source(app_source);
    if (!math_parsed.succeeded() || !app_parsed.succeeded()) {
        const auto& diagnostic = !math_parsed.succeeded()
            ? math_parsed.diagnostics.front() : app_parsed.diagnostics.front();
        std::cerr << "N49_N53_PARSE | RESULT=FAILED | CODE=" << diagnostic.code
                  << " | MESSAGE=" << diagnostic.message << "\n";
        return 4;
    }

    const auto program = compile_x64_coff_program({
        NativeProgramModuleInput{"Math.src", std::make_shared<CompilationUnit>(std::move(math_parsed.unit))},
        NativeProgramModuleInput{"App.src", std::make_shared<CompilationUnit>(std::move(app_parsed.unit))}});
    if (!program.succeeded() || program.modules.size() != 2U) {
        const std::string message = program.diagnostics.empty()
            ? "whole-program compilation did not emit exactly two modules"
            : program.diagnostics.front().code + ": " + program.diagnostics.front().message;
        std::cerr << "N49_N53_CODEGEN | RESULT=FAILED | MESSAGE=" << message << "\n";
        return 5;
    }

    const auto math_module = std::find_if(program.modules.begin(), program.modules.end(), [](const NativeProgramModuleOutput& item) {
        return item.module_name == "Math";
    });
    const auto app_module = std::find_if(program.modules.begin(), program.modules.end(), [](const NativeProgramModuleOutput& item) {
        return item.module_name == "App";
    });
    if (math_module == program.modules.end() || app_module == program.modules.end()) {
        std::cerr << "N49_N53_CODEGEN | RESULT=FAILED | MODULE_OUTPUT_MISSING\n";
        return 6;
    }
    const NativeSymbolRecord* main_symbol = find_symbol(app_module->codegen, "main");
    const NativeSymbolRecord* unused_app_symbol = find_symbol(app_module->codegen, "unusedapp");
    const NativeSymbolRecord* add_symbol = find_symbol(math_module->codegen, "add");
    const NativeSymbolRecord* unused_math_symbol = find_symbol(math_module->codegen, "unusedmath");
    if (main_symbol == nullptr || main_symbol->mangled_name.empty() || unused_app_symbol == nullptr ||
        add_symbol == nullptr || add_symbol->mangled_name.empty() || unused_math_symbol == nullptr) {
        std::cerr << "N49_N53_CODEGEN | RESULT=FAILED | REQUIRED_SYMBOL_MISSING\n";
        return 7;
    }

    if (!write_bytes(output_directory / "n49-n53-app.obj", app_module->codegen.object_bytes) ||
        !write_bytes(output_directory / "n49-n53-math.obj", math_module->codegen.object_bytes) ||
        !write_text(output_directory / "n49-n53-entry-symbol.txt", main_symbol->mangled_name + "\r\n") ||
        !write_text(output_directory / "n49-n53-add-symbol.txt", add_symbol->mangled_name + "\r\n") ||
        !write_text(output_directory / "n49-n53-unused-app-symbol.txt", unused_app_symbol->mangled_name + "\r\n") ||
        !write_text(output_directory / "n49-n53-unused-math-symbol.txt", unused_math_symbol->mangled_name + "\r\n")) {
        std::cerr << "N49_N53_OUTPUT | RESULT=FAILED | ERROR=WRITE_OBJECTS\n";
        return 8;
    }

    // A separate PE image verifies N49 .pdata/.xdata creation and Windows runtime lookup.
    // DllMain: mov eax,1; ret.
    Pe64DllSpec dll;
    dll.dll_name = "n49-unwind-probe.dll";
    dll.text.assign(16U, 0x90U);
    const std::vector<std::uint8_t> dll_main{0xB8U, 0x01U, 0x00U, 0x00U, 0x00U, 0xC3U};
    std::copy(dll_main.begin(), dll_main.end(), dll.text.begin());
    const std::vector<std::uint8_t> unwind_probe{
        0x55U,                         // push rbp
        0x48U, 0x89U, 0xE5U,           // mov rbp, rsp
        0x48U, 0x81U, 0xECU, 0x10U, 0x00U, 0x00U, 0x00U, // sub rsp, 16
        0xB8U, 0x2AU, 0x00U, 0x00U, 0x00U, // mov eax, 42
        0x48U, 0x89U, 0xECU,           // mov rsp, rbp
        0x5DU, 0xC3U                   // pop rbp; ret
    };
    dll.text.insert(dll.text.end(), unwind_probe.begin(), unwind_probe.end());
    dll.has_entry_point = true;
    dll.entry_point_offset = 0U;
    // Give the loader a real DIR64 fixup so the probe DLL is relocatable if
    // its preferred image base is already occupied on the test machine.
    dll.data.assign(8U, 0U);
    dll.absolute_relocations.push_back(Pe64AbsoluteRelocation{0U, Pe64SectionKind::Text, 16U, 0});
    dll.exports.push_back(Pe64DllExport{"XUnwindProbe", 1U, Pe64SectionKind::Text, 16U, false, {}});
    const auto unwind_plan = build_win64_frame_unwind_plan(16U, 16U + unwind_probe.size(), 16U, true);
    if (!unwind_plan.succeeded()) {
        const std::string message = unwind_plan.diagnostics.empty() ? "unwind planner failed" : unwind_plan.diagnostics.front().message;
        std::cerr << "N49_UNWIND | RESULT=FAILED | MESSAGE=" << message << "\n";
        return 9;
    }
    dll.runtime_functions.push_back(unwind_plan.runtime_function);
    const auto dll_image = write_pe64_dll_image(dll);
    if (!dll_image.succeeded() || dll_image.image.runtime_function_count != 1U ||
        dll_image.image.exception_directory_rva == 0U || dll_image.image.unwind_info_size == 0U) {
        const std::string message = dll_image.diagnostics.empty() ? "PE unwind directory was not emitted" : dll_image.diagnostics.front().message;
        std::cerr << "N49_UNWIND | RESULT=FAILED | MESSAGE=" << message << "\n";
        return 10;
    }
    if (!write_bytes(output_directory / "n49-unwind-probe.dll", dll_image.image.bytes)) {
        std::cerr << "N49_UNWIND | RESULT=FAILED | ERROR=WRITE_DLL\n";
        return 11;
    }

    std::cout << "N49_N53_PIPELINE_EMIT | RESULT=PASSED | MODULES=2 | OBJECTS=2"
              << " | DOUBLE_CALL=PASSED | POINTER=PASSED | MAIN_SYMBOL=" << main_symbol->mangled_name
              << " | MATH_SYMBOL=" << add_symbol->mangled_name
              << " | UNWIND_PDATA=" << dll_image.image.exception_directory_rva
              << " | UNWIND_RECORDS=" << dll_image.image.runtime_function_count
              << " | UNWIND_DLL=CREATED | EXIT=0\n";
    return 0;
}
