// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/linear_scan_allocator.hpp"
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
    if (!parsed.succeeded()) throw std::runtime_error("N54 parser failure: " + parsed.diagnostics.front().code);
    return std::move(parsed.unit);
}

bool has_bytes(const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& pattern) {
    return std::search(data.begin(), data.end(), pattern.begin(), pattern.end()) != data.end();
}

void test_n54_emitter_uses_volatile_registers(std::size_t index) {
    const auto unit = parse_unit(
        "module RegisterSmoke; public function Main(int seed) returns int { int value = seed + " +
        std::to_string(11U + (index % 7U)) + "; return (value + 14) * (value + 4); }");
    const auto result = compile_x64_coff_object(unit,
        X64CodegenOptions{"n54-register-use-" + std::to_string(index), make_coff_x86_64_object_target()});
    require(result.succeeded(), "register-allocated x64 object generation failed");
    require(result.stats.volatile_register_temporaries >= 3U,
            "x64 Emitter did not consume linear-scan volatile-register assignments");
    require(result.stats.stack_spilled_temporaries == 0U,
            "call-free expression unexpectedly spilled a live temporary to stack");
    require(result.stats.stack_temporary_bytes_avoided >= 24U,
            "register allocation did not report the eliminated temporary stack traffic");
    const auto& code = result.native_object.object_file.sections.front().contents;
    require(has_bytes(code, {0x49U, 0x89U, 0xC0U}) || has_bytes(code, {0x49U, 0x89U, 0xC1U}) ||
            has_bytes(code, {0x49U, 0x89U, 0xC2U}) || has_bytes(code, {0x49U, 0x89U, 0xC3U}),
            "generated code does not save an expression temporary in a caller-saved register");
}

void test_n54_call_crossing_spills_safely(std::size_t index) {
    const auto unit = parse_unit(
        "module CallPressure; private function Add(int a, int b) returns int { return a + b; } "
        "public function Main() returns int { return (10 + " + std::to_string(index % 5U) +
        ") + Add(20, 12); }");
    const auto result = compile_x64_coff_object(unit,
        X64CodegenOptions{"n54-call-spill-" + std::to_string(index), make_coff_x86_64_object_target()});
    require(result.succeeded(), "call-crossing temporary code generation failed");
    require(result.stats.stack_spilled_temporaries >= 1U,
            "live value crossing a call was not spilled before volatile registers are clobbered");
    require(result.stats.volatile_register_temporaries >= 1U,
            "callee arithmetic did not use an allocated volatile register");
    require(std::count_if(result.native_object.object_file.relocations.begin(), result.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
            "direct Add call lost its COFF REL32 relocation");
}

void test_n54_allocator_volatile_policy(std::size_t index) {
    const auto allocated = allocate_native_registers({
        NativeLiveInterval{static_cast<std::uint32_t>(100U + index), 0U, 10U, NativeRegisterClass::GeneralPurpose, false},
        NativeLiveInterval{static_cast<std::uint32_t>(200U + index), 2U, 8U, NativeRegisterClass::GeneralPurpose, false},
        NativeLiveInterval{static_cast<std::uint32_t>(300U + index), 11U, 14U, NativeRegisterClass::GeneralPurpose, false}},
        NativeRegisterAllocationOptions{true, true, false, false});
    require(allocated.succeeded && allocated.assignments.size() == 3U,
            "caller-saved register allocation did not return all intervals");
    const auto& first = allocated.assignments[0U];
    const auto& second = allocated.assignments[1U];
    const auto& third = allocated.assignments[2U];
    require(!first.spilled && !second.spilled && !third.spilled,
            "low-pressure volatile allocation unexpectedly spilled");
    require(first.register_index >= 8U && first.register_index <= 11U &&
            second.register_index >= 8U && second.register_index <= 11U &&
            third.register_index >= 8U && third.register_index <= 11U,
            "caller-saved GP assignment is outside R8-R11");
    require(first.register_index != second.register_index,
            "overlapping live intervals share the same volatile register");
    require(third.register_index == first.register_index,
            "linear scan did not reuse a register after a prior interval expired");
    require(!first.requires_callee_save && !second.requires_callee_save && !third.requires_callee_save,
            "volatile register assignment incorrectly requires a callee-save prologue");
}

void test_n54_allocator_spills_call_crossing(std::size_t index) {
    const auto allocated = allocate_native_registers({
        NativeLiveInterval{static_cast<std::uint32_t>(500U + index), 0U, 100U, NativeRegisterClass::GeneralPurpose, true},
        NativeLiveInterval{static_cast<std::uint32_t>(600U + index), 1U, 5U, NativeRegisterClass::GeneralPurpose, false}},
        NativeRegisterAllocationOptions{true, true, false, false});
    require(allocated.succeeded && allocated.assignments.size() == 2U,
            "call-crossing allocator result is incomplete");
    const auto found = std::find_if(allocated.assignments.begin(), allocated.assignments.end(), [&](const auto& item) {
        return item.value_id == static_cast<std::uint32_t>(500U + index);
    });
    require(found != allocated.assignments.end() && found->spilled && !found->requires_callee_save,
            "call-crossing value must use a stack spill under caller-saved allocation policy");
    require(allocated.spill_slot_count >= 1U,
            "call-crossing volatile interval did not reserve a spill slot");
}

} // namespace

void run_n54_register_codegen_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 500U; ++index) {
        run_case("N54A_emitter_volatile_registers_" + std::to_string(index),
            [index] { test_n54_emitter_uses_volatile_registers(index); }, passed, failed);
        run_case("N54B_call_crossing_spill_" + std::to_string(index),
            [index] { test_n54_call_crossing_spills_safely(index); }, passed, failed);
        run_case("N54C_allocator_volatile_policy_" + std::to_string(index),
            [index] { test_n54_allocator_volatile_policy(index); }, passed, failed);
        run_case("N54D_allocator_call_spill_" + std::to_string(index),
            [index] { test_n54_allocator_spills_call_crossing(index); }, passed, failed);
    }
}
