// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/binary_object.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/x64_codegen.hpp"
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
    if (!parsed.succeeded())
        throw std::runtime_error("N72-N77 parser failure: " + parsed.diagnostics.front().code + " " +
            parsed.diagnostics.front().message);
    return std::move(parsed.unit);
}

X64CodegenResult compile_one(const std::string& source, const std::string& id) {
    return compile_x64_coff_object(parse_unit(source),
        X64CodegenOptions{id, make_coff_x86_64_object_target()});
}

std::string nested_integer_expression(std::size_t depth) {
    std::string expression = "seed";
    for (std::size_t index = 0U; index < depth; ++index)
        expression = "(seed + " + expression + ")";
    return expression;
}

std::string nested_float_expression(std::size_t depth) {
    std::string expression = "seed";
    for (std::size_t index = 0U; index < depth; ++index)
        expression = "(seed + " + expression + ")";
    return expression;
}

bool contains_bytes(const std::vector<std::uint8_t>& bytes,
                    const std::vector<std::uint8_t>& pattern) {
    return std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}

void test_n72_ast_register_ir_bridge(std::size_t index) {
    const auto result = compile_one(
        "module N72; public function Main(int seed) returns int { return (seed + " +
        std::to_string(1U + index % 9U) + ") * (seed + 3); }", "n72-ast-ir-" + std::to_string(index));
    require(result.succeeded(), "N72 AST-to-native-register-IR lowering failed");
    require(result.stats.native_register_values_analyzed == 3U,
            "N72 analysis did not see each non-logical binary expression exactly once");
}

void test_n73_volatile_first_allocation(std::size_t index) {
    const auto result = compile_one(
        "module N73; public function Main(int seed) returns int { return (seed + " +
        std::to_string(2U + index % 7U) + ") * (seed + 5); }", "n73-volatile-first-" + std::to_string(index));
    require(result.succeeded(), "N73 volatile-first x64 allocation failed");
    require(result.stats.volatile_register_temporaries >= 1U,
            "N73 did not consume the caller-saved scratch-register pool first");
    require(result.stats.native_persistent_gp_register_assignments == 1U,
            "N73 persistent source parameter was not assigned in its nonvolatile allocation domain");
    require(result.stats.callee_saved_gp_registers <=
                result.stats.native_persistent_gp_register_assignments,
            "N73 expression allocation consumed an extra nonvolatile GP register beyond the persistent domain");
    require(result.stats.native_register_values_analyzed >= 3U,
            "N73 codegen bypassed native register IR analysis");
}

void test_n74_fixed_gp_spill_slots(std::size_t index) {
    const auto result = compile_one(
        "module N74; public function Main(int seed) returns int { return " +
        nested_integer_expression(16U + index % 7U) + "; }", "n74-gp-spill-" + std::to_string(index));
    require(result.succeeded(), "N74 high-pressure GP code generation failed");
    require(result.stats.native_spill_slots > 0U && result.stats.stack_spilled_temporaries > 0U,
            "N74 pressure did not produce fixed frame spill slots");
    require(result.stats.callee_saved_gp_registers > 0U,
            "N74 allocator did not fall back to Win64 nonvolatile GP registers");
    require((result.stats.stack_frame_bytes % 16U) == 0U,
            "N74 frame containing locals/spills/saves is not 16-byte aligned");
    const auto& code = result.native_object.object_file.sections.front().contents;
    require(contains_bytes(code, {0x48U, 0x89U, 0x85U}) && contains_bytes(code, {0x48U, 0x8BU, 0x85U}),
            "N74 did not emit RBP-relative spill stores and reloads");
}

void test_n75_nonvolatile_xmm_save_restore(std::size_t index) {
    const auto result = compile_one(
        "module N75; public function Main(double seed) returns double { return " +
        nested_float_expression(12U + index % 6U) + "; }", "n75-xmm-save-" + std::to_string(index));
    if (!result.succeeded()) {
        std::string detail = "N75 high-pressure XMM code generation failed";
        if (!result.diagnostics.empty()) detail += ": " + result.diagnostics.front().code + " " + result.diagnostics.front().message;
        throw std::runtime_error(detail);
    }
    require(result.stats.callee_saved_xmm_registers > 0U,
            "N75 allocator did not use Win64 nonvolatile XMM6-XMM15 under pressure");
    require((result.stats.stack_frame_bytes % 16U) == 0U,
            "N75 frame with XMM saves is not 16-byte aligned");
    const auto& code = result.native_object.object_file.sections.front().contents;
    require(contains_bytes(code, {0xF3U, 0x44U, 0x0FU, 0x7FU}) ||
            contains_bytes(code, {0xF3U, 0x0FU, 0x7FU}),
            "N75 prologue did not encode MOVDQU stores for nonvolatile XMM registers");
    require(contains_bytes(code, {0xF3U, 0x44U, 0x0FU, 0x6FU}) ||
            contains_bytes(code, {0xF3U, 0x0FU, 0x6FU}),
            "N75 epilogue did not encode MOVDQU restores for nonvolatile XMM registers");
}

void test_n76_call_crossing_uses_frame_spill(std::size_t index) {
    const auto result = compile_one(
        "module N76; public function Add(int a, int b) returns int; "
        "public function Main(int seed) returns int { return (seed + " +
        std::to_string(4U + index % 5U) + ") + Add(20, 22); }",
        "n76-call-home-" + std::to_string(index));
    require(result.succeeded(), "N76 call-crossing register preservation failed");
    require(std::count_if(result.native_object.object_file.relocations.begin(), result.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
            "N76 direct call lost its COFF REL32 relocation");
    require(result.stats.native_spill_slots > 0U && result.stats.stack_spilled_temporaries > 0U,
            "N76 call-crossing value did not use a fixed frame spill slot");
    require((result.stats.stack_frame_bytes % 16U) == 0U,
            "N76 call-containing function lost Win64 stack alignment");
}

void test_n77_coff_object_roundtrip_is_deterministic(std::size_t index) {
    const std::string source =
        "module N77; public function Main(int seed) returns int { return " +
        nested_integer_expression(3U + index % 5U) + "; }";
    const auto unit = parse_unit(source);
    const X64CodegenOptions options{"n77-deterministic", make_coff_x86_64_object_target()};
    const auto first = compile_x64_coff_object(unit, options);
    const auto second = compile_x64_coff_object(unit, options);
    require(first.succeeded() && second.succeeded(), "N77 object emission failed");
    require(first.object_bytes == second.object_bytes,
            "N77 repeated compilation did not produce byte-identical COFF objects");
    const auto parsed = read_binary_object(first.object_bytes);
    require(parsed.succeeded(), "N77 emitted COFF object failed binary roundtrip validation");
    require(parsed.object_file.sections.size() == first.native_object.object_file.sections.size(),
            "N77 COFF roundtrip changed the section count");
}

} // namespace

void run_n72_n77_register_codegen_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 2000U; ++index) {
        run_case("N72A_ast_register_ir_bridge_" + std::to_string(index),
            [index] { test_n72_ast_register_ir_bridge(index); }, passed, failed);
        run_case("N73A_volatile_first_allocation_" + std::to_string(index),
            [index] { test_n73_volatile_first_allocation(index); }, passed, failed);
        run_case("N74A_fixed_gp_spill_slots_" + std::to_string(index),
            [index] { test_n74_fixed_gp_spill_slots(index); }, passed, failed);
        run_case("N75A_nonvolatile_xmm_save_restore_" + std::to_string(index),
            [index] { test_n75_nonvolatile_xmm_save_restore(index); }, passed, failed);
        run_case("N76A_call_crossing_frame_spill_" + std::to_string(index),
            [index] { test_n76_call_crossing_uses_frame_spill(index); }, passed, failed);
        run_case("N77A_coff_deterministic_roundtrip_" + std::to_string(index),
            [index] { test_n77_coff_object_roundtrip_is_deterministic(index); }, passed, failed);
    }
}
