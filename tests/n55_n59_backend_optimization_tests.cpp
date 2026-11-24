// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/linear_scan_allocator.hpp"
#include "xfrontend/native_program_pipeline.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void run_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
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
    if (!parsed.succeeded()) throw std::runtime_error("N55-N59 parser failure: " + parsed.diagnostics.front().code);
    return std::move(parsed.unit);
}

X64CodegenResult compile_one(const std::string& source, const std::string& id) {
    const auto unit = parse_unit(source);
    return compile_x64_coff_object(unit,
        X64CodegenOptions{id, make_coff_x86_64_object_target()});
}

void test_n55_xmm_temporary_is_consumed(std::size_t index) {
    const std::string source =
        "module XmmAllocation; public function Main(double seed) returns double { "
        "double value = seed + " + std::to_string(1U + index % 9U) + ".0; "
        "return (value + 2.5) * (value + 3.5); }";
    const auto result = compile_one(source, "n55-xmm-register-" + std::to_string(index));
    require(result.succeeded(), "floating-point x64 code generation failed");
    require(result.stats.xmm_register_temporaries >= 1U,
            "XMM linear-scan assignment was not consumed by floating-point emitter");
    require(result.stats.xmm_stack_spilled_temporaries == 0U,
            "call-free low-pressure FP expression unexpectedly spilled");
    require(result.object_bytes.size() > 0U, "COFF object is empty");
}

void test_n55_xmm_allocator_reserves_xmm0(std::size_t index) {
    const auto result = allocate_native_registers({
        NativeLiveInterval{static_cast<std::uint32_t>(1000U + index), 0U, 8U, NativeRegisterClass::FloatingPoint, false},
        NativeLiveInterval{static_cast<std::uint32_t>(2000U + index), 1U, 5U, NativeRegisterClass::FloatingPoint, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.assignments.size() == 2U, "XMM allocator did not return both assignments");
    for (const auto& assignment : result.assignments) {
        require(!assignment.spilled, "low-pressure XMM interval unexpectedly spilled");
        require(assignment.register_class == NativeRegisterClass::FloatingPoint, "FP interval changed register class");
        require(assignment.register_index >= 1U && assignment.register_index <= 5U,
                "XMM0 return register was not reserved");
    }
    require(result.assignments[0U].register_index != result.assignments[1U].register_index,
            "overlapping XMM live intervals share a register");
}

void test_n55_call_crossing_fp_temporary_spills(std::size_t index) {
    const std::string source =
        "module XmmCall; private function Add(double a, double b) returns double { return a + b; } "
        "public function Main() returns double { return (" + std::to_string(10U + index % 4U) +
        ".0 + 2.0) + Add(20.0, 10.0); }";
    const auto result = compile_one(source, "n55-xmm-call-spill-" + std::to_string(index));
    require(result.succeeded(), "FP call-crossing codegen failed");
    require(result.stats.xmm_stack_spilled_temporaries >= 1U,
            "FP temporary live across a call was not spilled safely");
    require(std::count_if(result.native_object.object_file.relocations.begin(), result.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
            "FP direct call lost its COFF relocation");
}

void test_n55_xmm_register_policy_reuses_expired(std::size_t index) {
    const auto result = allocate_native_registers({
        NativeLiveInterval{static_cast<std::uint32_t>(3000U + index), 0U, 2U, NativeRegisterClass::FloatingPoint, false},
        NativeLiveInterval{static_cast<std::uint32_t>(4000U + index), 3U, 6U, NativeRegisterClass::FloatingPoint, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.assignments.size() == 2U, "XMM register reuse allocation failed");
    require(result.assignments[0U].register_index == result.assignments[1U].register_index,
            "expired XMM register was not reused");
}

void test_n56_sequential_scopes_reuse_local_slot(std::size_t index) {
    const auto result = compile_one(
        "module StackReuse; public function Main() returns int { { int first = " +
        std::to_string(index % 31U) + "; } { double second = 3.5; } return 42; }",
        "n56-local-slot-reuse-" + std::to_string(index));
    require(result.succeeded(), "sequential-scope codegen failed");
    require(result.stats.local_stack_slots_reused >= 1U,
            "local stack slot from an expired lexical scope was not reused");
    require(result.stats.local_stack_slots_peak == 1U,
            "non-overlapping lexical locals consumed more than one stack slot");
    require((result.stats.stack_frame_bytes % 16U) == 0U, "Win64 frame lost 16-byte alignment");
}

void test_n56_nested_live_local_is_not_reused(std::size_t index) {
    const auto result = compile_one(
        "module StackNested; public function Main() returns int { int outer = " +
        std::to_string(10U + index % 9U) + "; { int inner = 7; outer = outer + inner; } return outer; }",
        "n56-nested-live-local-" + std::to_string(index));
    require(result.succeeded(), "nested-scope codegen failed");
    require(result.stats.local_stack_slots_peak >= 2U,
            "nested live locals incorrectly shared the same stack slot");
}

void test_n56_mixed_type_slot_reuse(std::size_t index) {
    const auto result = compile_one(
        "module StackMixed; public function Main() returns int { { double fp = 1.25; } "
        "{ int value = " + std::to_string(30U + index % 7U) + "; return value + 12; } }",
        "n56-mixed-slot-reuse-" + std::to_string(index));
    require(result.succeeded(), "mixed-type stack reuse failed");
    require(result.stats.local_stack_slots_reused >= 1U, "8-byte slot was not reused across scalar types");
    require((result.stats.stack_frame_bytes % 16U) == 0U, "mixed stack frame is not Win64 aligned");
}

void test_n56_frame_size_tracks_peak_slots(std::size_t index) {
    const auto result = compile_one(
        "module StackPeak; public function Main() returns int { { int a = 1; int b = 2; } "
        "{ int c = " + std::to_string(index % 13U) + "; } return 42; }",
        "n56-frame-peak-" + std::to_string(index));
    require(result.succeeded(), "peak frame analysis failed");
    require(result.stats.local_stack_slots_peak == 2U, "frame peak did not match max simultaneously-live locals");
    require(result.stats.stack_frame_bytes == 16U, "frame size did not use peak slot count with 16-byte alignment");
}

void test_n57_nested_integer_constants_fold(std::size_t index) {
    const auto result = compile_one(
        "module ConstantFold; public function Main() returns int { return ((" +
        std::to_string(7U + index % 5U) + " + 5) * (3 + 4)) - 2; }",
        "n57-integer-fold-" + std::to_string(index));
    require(result.succeeded(), "integer constant-fold codegen failed");
    require(result.stats.constant_folded_expressions >= 1U,
            "nested integer constant expression was not folded");
    const auto text_bytes = std::accumulate(result.native_object.object_file.sections.begin(), result.native_object.object_file.sections.end(), std::size_t{0U}, [](std::size_t total, const ObjectSection& section) { return total + (section.kind == ObjectSectionKind::Text ? section.contents.size() : 0U); });
    require(text_bytes < 128U, "folded constant text section unexpectedly large");
}

void test_n57_float_constant_fold(std::size_t index) {
    const auto result = compile_one(
        "module FloatFold; public function Main() returns double { return (" +
        std::to_string(1U + index % 8U) + ".5 + 2.5) * 2.0; }",
        "n57-float-fold-" + std::to_string(index));
    require(result.succeeded(), "floating constant-fold codegen failed");
    require(result.stats.constant_folded_expressions >= 1U, "floating constant expression was not folded");
}

void test_n57_division_by_zero_is_not_folded(std::size_t index) {
    (void)index;
    const auto result = compile_one(
        "module NoZeroFold; public function Main() returns int { return 7 / 0; }", "n57-zero-divisor");
    require(result.succeeded(), "zero-divisor expression should remain a runtime operation in this stage");
    require(result.stats.constant_folded_expressions == 0U, "division by zero was folded unsafely");
}

void test_n57_integer_overflow_is_not_folded(std::size_t index) {
    (void)index;
    const auto result = compile_one(
        "module OverflowFold; public function Main() returns int { return 9223372036854775807 + 1; }",
        "n57-overflow-not-folded");
    require(result.succeeded(), "overflowing constant expression must fall back to runtime codegen");
    require(result.stats.constant_folded_expressions == 0U, "signed overflow was folded with undefined behavior");
}

void test_n58_loop_codegen_metrics(std::size_t index) {
    const auto result = compile_one(
        "module BenchmarkLoop; public function Main() returns int { int i = 0; int sum = 0; "
        "while (i < " + std::to_string(1000U + index % 100U) + ") { sum = sum + i; i = i + 1; } "
        "if (sum > 0) { return 42; } else { return 1; } }",
        "n58-loop-metrics-" + std::to_string(index));
    require(result.succeeded(), "benchmark loop codegen failed");
    require(!result.object_bytes.empty(), "benchmark loop did not produce a COFF object");
    require(result.stats.stack_frame_bytes >= 16U, "benchmark locals did not allocate an aligned frame");
}

void test_n58_benchmark_pair_has_deterministic_code_size(std::size_t index) {
    const std::string source =
        "module BenchmarkPair; public function Main() returns int { int value = " +
        std::to_string(40U + index % 3U) + "; return (value + 1) * 1; }";
    const auto first = compile_one(source, "n58-bench-a-" + std::to_string(index));
    const auto second = compile_one(source, "n58-bench-b-" + std::to_string(index));
    require(first.succeeded() && second.succeeded(), "benchmark determinism compilation failed");
    require(first.object_bytes.size() == second.object_bytes.size(), "identical benchmark source changed object size");
    require(first.stats.peephole_optimizations >= 1U, "identity peephole was not applied");
}

void test_n58_constant_benchmark_reports_folds(std::size_t index) {
    const auto result = compile_one(
        "module BenchmarkConst; public function Main() returns int { return (" +
        std::to_string(index % 100U) + " + 42) * 3; }",
        "n58-bench-fold-" + std::to_string(index));
    require(result.succeeded(), "constant benchmark codegen failed");
    require(result.stats.constant_folded_expressions >= 1U, "benchmark did not report compile-time folds");
}

void test_n58_code_size_metric_nonzero(std::size_t index) {
    const auto result = compile_one(
        "module BenchmarkSize; public function Main() returns int { int value = " +
        std::to_string(index % 17U) + "; return value + 42; }",
        "n58-code-size-" + std::to_string(index));
    require(result.succeeded(), "code-size metric compilation failed");
    require(result.object_bytes.size() > 0U, "object byte metric is zero");
}

void test_n59_two_module_fp_pointer_pipeline(std::size_t index) {
    const std::string math_source =
        "module Math; public function Add(double a, double b) returns double { return a + b; }";
    const std::string app_source =
        "module App; import Math; public function Main() returns int { "
        "double total = Math.Add(" + std::to_string(10U + index % 5U) + ".0, 32.0); "
        "int value = 17; int* pointer = &value; *pointer = *pointer + 25; "
        "if (total == " + std::to_string(42U + index % 5U) + ".0 && *pointer == 42) { return 42; } else { return 1; } }";
    auto math = parse_unit(math_source);
    auto app = parse_unit(app_source);
    const auto program = compile_x64_coff_program({
        NativeProgramModuleInput{"Math.src", std::make_shared<CompilationUnit>(std::move(math))},
        NativeProgramModuleInput{"App.src", std::make_shared<CompilationUnit>(std::move(app))}});
    require(program.succeeded(), program.diagnostics.empty() ? "two-module native pipeline failed" :
        program.diagnostics.front().code + ": " + program.diagnostics.front().message);
    require(program.modules.size() == 2U, "native pipeline did not emit two modules");
    const auto app_module = std::find_if(program.modules.begin(), program.modules.end(), [](const auto& item) {
        return item.module_name == "App";
    });
    const auto math_module = std::find_if(program.modules.begin(), program.modules.end(), [](const auto& item) {
        return item.module_name == "Math";
    });
    require(app_module != program.modules.end() && math_module != program.modules.end(), "App or Math object is missing");
    require(!app_module->codegen.object_bytes.empty() && !math_module->codegen.object_bytes.empty(),
            "multi-module COFF output is empty");
    require(!app_module->codegen.native_object.object_file.relocations.empty(),
            "qualified Math.Add call did not emit a relocation");
}

void test_n59_dce_and_allocator_pipeline(std::size_t index) {
    const auto result = compile_one(
        "module NativeIntegrated; private function Bump(int value) returns int { return value + 1; } "
        "private function Unused() returns int { return 99; } "
        "public function Main(double seed) returns int { double value = (seed + " +
        std::to_string(index % 4U) + ".5) * 2.0; int count = 17; int* pointer = &count; "
        "*pointer = Bump(*pointer + 24); if (value > 0.0 && *pointer == 42) { return 42; } else { return 1; } }",
        "n59-integrated-pipeline-" + std::to_string(index));
    require(result.succeeded(), "integrated N59 codegen failed");
    require(!result.native_object.object_file.sections.empty(), "integrated COFF sections missing");
    require(!result.native_object.object_file.relocations.empty(), "integrated DCE/call pipeline lacks relocations");
    require(result.stats.xmm_register_temporaries >= 1U, "integrated FP path did not consume XMM allocator");
}

void test_n59_import_signature_mismatch_is_rejected(std::size_t index) {
    (void)index;
    auto math = parse_unit("module Math; public function Add(double a, double b) returns double { return a + b; }");
    auto app = parse_unit("module App; import Math; public function Main() returns int { int a = Math.Add(1, 2); return a; }");
    const auto result = compile_x64_coff_program({
        NativeProgramModuleInput{"Math.src", std::make_shared<CompilationUnit>(std::move(math))},
        NativeProgramModuleInput{"App.src", std::make_shared<CompilationUnit>(std::move(app))}});
    require(!result.succeeded(), "cross-module floating/integer signature mismatch was accepted");
}

void test_n59_emission_is_deterministic(std::size_t index) {
    const std::string source =
        "module Deterministic; public function Main() returns int { double a = " +
        std::to_string(1U + index % 9U) + ".5; if ((a + 2.5) * 2.0 > 0.0) { return 42; } else { return 1; } }";
    const auto first = compile_one(source, "n59-deterministic-a-" + std::to_string(index));
    const auto second = compile_one(source, "n59-deterministic-b-" + std::to_string(index));
    require(first.succeeded() && second.succeeded(), "deterministic pipeline compile failed");
    require(first.object_bytes == second.object_bytes, "identical AST produced non-deterministic COFF bytes");
}

} // namespace

void run_n55_n59_backend_optimization_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 500U; ++index) {
        run_case("N55A_xmm_emitter_" + std::to_string(index), [index] { test_n55_xmm_temporary_is_consumed(index); }, passed, failed);
        run_case("N55B_xmm_allocator_policy_" + std::to_string(index), [index] { test_n55_xmm_allocator_reserves_xmm0(index); }, passed, failed);
        run_case("N55C_xmm_call_spill_" + std::to_string(index), [index] { test_n55_call_crossing_fp_temporary_spills(index); }, passed, failed);
        run_case("N55D_xmm_register_reuse_" + std::to_string(index), [index] { test_n55_xmm_register_policy_reuses_expired(index); }, passed, failed);

        run_case("N56A_scope_slot_reuse_" + std::to_string(index), [index] { test_n56_sequential_scopes_reuse_local_slot(index); }, passed, failed);
        run_case("N56B_nested_scope_safety_" + std::to_string(index), [index] { test_n56_nested_live_local_is_not_reused(index); }, passed, failed);
        run_case("N56C_mixed_type_slot_reuse_" + std::to_string(index), [index] { test_n56_mixed_type_slot_reuse(index); }, passed, failed);
        run_case("N56D_stack_frame_peak_" + std::to_string(index), [index] { test_n56_frame_size_tracks_peak_slots(index); }, passed, failed);

        run_case("N57A_integer_constant_fold_" + std::to_string(index), [index] { test_n57_nested_integer_constants_fold(index); }, passed, failed);
        run_case("N57B_float_constant_fold_" + std::to_string(index), [index] { test_n57_float_constant_fold(index); }, passed, failed);
        run_case("N57C_zero_divisor_guard_" + std::to_string(index), [index] { test_n57_division_by_zero_is_not_folded(index); }, passed, failed);
        run_case("N57D_signed_overflow_guard_" + std::to_string(index), [index] { test_n57_integer_overflow_is_not_folded(index); }, passed, failed);

        run_case("N58A_loop_benchmark_metrics_" + std::to_string(index), [index] { test_n58_loop_codegen_metrics(index); }, passed, failed);
        run_case("N58B_benchmark_determinism_" + std::to_string(index), [index] { test_n58_benchmark_pair_has_deterministic_code_size(index); }, passed, failed);
        run_case("N58C_constant_benchmark_" + std::to_string(index), [index] { test_n58_constant_benchmark_reports_folds(index); }, passed, failed);
        run_case("N58D_object_size_metric_" + std::to_string(index), [index] { test_n58_code_size_metric_nonzero(index); }, passed, failed);

        run_case("N59A_two_module_fp_pointer_" + std::to_string(index), [index] { test_n59_two_module_fp_pointer_pipeline(index); }, passed, failed);
        run_case("N59B_integrated_dce_allocator_" + std::to_string(index), [index] { test_n59_dce_and_allocator_pipeline(index); }, passed, failed);
        run_case("N59C_import_signature_guard_" + std::to_string(index), [index] { test_n59_import_signature_mismatch_is_rejected(index); }, passed, failed);
        run_case("N59D_deterministic_coff_" + std::to_string(index), [index] { test_n59_emission_is_deterministic(index); }, passed, failed);
    }
}
