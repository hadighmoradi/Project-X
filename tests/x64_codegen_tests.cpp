#include "xfrontend/x64_codegen.hpp"
#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

void n48_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n48_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(id)) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(id, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(id, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
}

CompilationUnit parse_unit(const std::string& source) {
    auto parsed = parse_source(source);
    if (!parsed.succeeded())
        throw std::runtime_error("N48 source parse failed: " + parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message);
    return std::move(parsed.unit);
}

void compile_literal_case(std::size_t index) {
    const std::string source = "module App; public function Main() returns int { return " +
        std::to_string(static_cast<long long>(index) - 250LL) + "; }";
    const auto result = compile_x64_coff_object(parse_unit(source), X64CodegenOptions{"n48_literal", make_coff_x86_64_object_target()});
    n48_require(result.succeeded(), "integer literal did not lower to a valid COFF object");
    n48_require(std::count_if(result.native_object.object_file.sections.begin(), result.native_object.object_file.sections.end(), [](const ObjectSection& section) { return section.kind == ObjectSectionKind::Text; }) == 1U, "literal program must emit one function text section");
    n48_require(result.native_object.object_file.sections[0U].comdat, "Win64 function section must be COMDAT");
    n48_require(result.native_object.object_file.sections[0U].contents.size() >= 20U, "machine-code prologue/body/epilogue is unexpectedly short");
    n48_require(result.object_bytes.size() > result.native_object.object_file.sections[0U].contents.size(), "serialized COFF object is missing headers/symbol data");
}

void compile_call_arithmetic_case(std::size_t index) {
    const auto unit = parse_unit(
        "module App;\n"
        "public function Main() returns int { int left = 17; int right = 25; return Add(left, right); }\n"
        "private function Add(int left, int right) returns int { return left * 2 + right / 1 - left % 2; }\n");
    const auto result = compile_x64_coff_object(unit, X64CodegenOptions{"n48_call_" + std::to_string(index), make_coff_x86_64_object_target()});
    n48_require(result.succeeded(), "Win64 function call or arithmetic lowering failed");
    n48_require(std::count_if(result.native_object.object_file.sections.begin(), result.native_object.object_file.sections.end(), [](const ObjectSection& section) { return section.kind == ObjectSectionKind::Text; }) == 2U, "Main and Add must each have a separate COMDAT text section");
    n48_require(std::count_if(result.native_object.object_file.relocations.begin(), result.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1, "direct function call must produce one COFF REL32 relocation");
    n48_require(result.native_object.object_file.relocations.front().kind == ObjectRelocationKind::CoffRelative32,
                "Win64 call must use AMD64 COFF REL32 relocation");
    const auto roundtrip = read_binary_object(result.object_bytes);
    n48_require(roundtrip.succeeded(), "N48 COFF object must survive binary roundtrip");
    n48_require(std::count_if(roundtrip.object_file.relocations.begin(), roundtrip.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1, "COFF roundtrip lost call relocation");
}

void compile_control_flow_case(std::size_t index) {
    const auto unit = parse_unit(
        "module Flow;\n"
        "public function Main() returns int {\n"
        "  int value = " + std::to_string(index % 11U) + ";\n"
        "  if (value < 5) { value = value + 7; } else { value = value - 2; }\n"
        "  while (value > 9) { value = value - 1; }\n"
        "  return value;\n"
        "}\n");
    const auto result = compile_x64_coff_object(unit, X64CodegenOptions{"n48_flow_" + std::to_string(index), make_coff_x86_64_object_target()});
    n48_require(result.succeeded(), "if/else/while x64 lowering failed");
    const auto& code = result.native_object.object_file.sections.front().contents;
    n48_require(std::find(code.begin(), code.end(), 0x0FU) != code.end(), "conditional branch opcode was not emitted");
    n48_require(std::find(code.begin(), code.end(), 0xE9U) != code.end(), "control-flow jump opcode was not emitted");
}

void compile_four_arg_and_external_case(std::size_t index) {
    const auto four_arg = parse_unit(
        "module Abi;\n"
        "public function Main() returns int { return Sum4(1, 2, 3, 4); }\n"
        "private function Sum4(int a, int b, int c, int d) returns int { return a + b + c + d; }\n");
    const auto four_result = compile_x64_coff_object(four_arg,
        X64CodegenOptions{"n48_four_arg_" + std::to_string(index), make_coff_x86_64_object_target()});
    n48_require(four_result.succeeded(), "four-register Win64 call lowering failed");
    n48_require(std::count_if(four_result.native_object.object_file.relocations.begin(), four_result.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
                "four-register call must emit one relocation");

    const auto external = parse_unit(
        "module ExternalApp;\n"
        "public function External(int value) returns int;\n"
        "public function Main() returns int { return External(7); }\n");
    const auto external_result = compile_x64_coff_object(external,
        X64CodegenOptions{"n48_external_" + std::to_string(index), make_coff_x86_64_object_target()});
    n48_require(external_result.succeeded(), "declared external function must lower to an unresolved COFF symbol");
    n48_require(std::count_if(external_result.native_object.object_file.relocations.begin(), external_result.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
                "external function call must emit one relocation");
    n48_require(std::any_of(external_result.native_object.object_file.symbols.begin(),
        external_result.native_object.object_file.symbols.end(), [](const ObjectSymbol& symbol) {
            return symbol.kind == ObjectSymbolKind::Function && !symbol.defined;
        }), "external function call must preserve an undefined COFF function symbol");
}

void reject_unsupported_case(std::size_t index) {
    const auto float_unit = parse_unit("module FloatSmoke; public function Main() returns double { double value = 1.5; return value + 2.5; }");
    const auto float_result = compile_x64_coff_object(float_unit, X64CodegenOptions{"n48_float_extension_" + std::to_string(index), make_coff_x86_64_object_target()});
    n48_require(float_result.succeeded(), "N51 floating-point extension must preserve N48 backend entry point");
    const std::vector<std::uint8_t> add_sd{0xF2U, 0x0FU, 0x58U};
    const bool has_scalar_fp = std::any_of(float_result.native_object.object_file.sections.begin(), float_result.native_object.object_file.sections.end(), [&add_sd](const ObjectSection& section) {
        return std::search(section.contents.begin(), section.contents.end(), add_sd.begin(), add_sd.end()) != section.contents.end();
    });
    n48_require(has_scalar_fp, "double expression must lower to a scalar SSE2 instruction");

    const auto abi_unit = parse_unit("module Bad; public function Main(int a, int b, int c, int d, int e) returns int { return a; }");
    const auto abi_result = compile_x64_coff_object(abi_unit, X64CodegenOptions{"n48_bad_abi_" + std::to_string(index), make_coff_x86_64_object_target()});
    n48_require(!abi_result.succeeded() && !abi_result.diagnostics.empty(), "more than four Win64 register arguments must be rejected in N48 initial subset");

    const auto missing_return = parse_unit("module Bad; public function Main() returns int { int value = 1; }");
    const auto return_result = compile_x64_coff_object(missing_return, X64CodegenOptions{"n48_bad_return_" + std::to_string(index), make_coff_x86_64_object_target()});
    n48_require(!return_result.succeeded() && !return_result.diagnostics.empty(), "non-void fallthrough must be rejected, not silently return zero");
}

} // namespace

void run_x64_codegen_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 400U; ++index) {
        n48_case("N48A_literal_" + std::to_string(index), [index] { compile_literal_case(index); }, passed, failed);
        n48_case("N48B_call_arithmetic_" + std::to_string(index), [index] { compile_call_arithmetic_case(index); }, passed, failed);
        n48_case("N48C_control_flow_" + std::to_string(index), [index] { compile_control_flow_case(index); }, passed, failed);
        n48_case("N48D_win64_abi_and_external_" + std::to_string(index), [index] { compile_four_arg_and_external_case(index); }, passed, failed);
        n48_case("N48E_reject_unsupported_" + std::to_string(index), [index] { reject_unsupported_case(index); }, passed, failed);
    }
}
