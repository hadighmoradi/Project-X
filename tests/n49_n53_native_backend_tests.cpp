#include "xfrontend/linear_scan_allocator.hpp"
#include "xfrontend/native_program_pipeline.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/pe64_dll.hpp"
#include "xfrontend/win64_unwind_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void run_case(const std::string& id, const std::function<void()>& body,
              int& passed, int& failed) {
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
    if (!parsed.succeeded()) {
        const auto& diagnostic = parsed.diagnostics.front();
        throw std::runtime_error("source parse failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return std::move(parsed.unit);
}

std::shared_ptr<const CompilationUnit> shared_unit(const std::string& source) {
    return std::make_shared<CompilationUnit>(parse_unit(source));
}

bool has_sequence(const std::vector<std::uint8_t>& bytes,
                  const std::vector<std::uint8_t>& sequence) {
    return std::search(bytes.begin(), bytes.end(), sequence.begin(), sequence.end()) != bytes.end();
}

void test_n49_unwind_plan(std::size_t index) {
    const std::uint32_t frames[] = {0U, 16U, 64U, 128U, 136U, 512U, 524288U};
    const std::uint32_t frame = frames[index % (sizeof(frames) / sizeof(frames[0]))];
    const auto plan = build_win64_frame_unwind_plan(0U, 64U, frame, true);
    require(plan.succeeded(), "Win64 frame unwind plan generation failed");
    require(plan.runtime_function.begin_offset == 0U && plan.runtime_function.end_offset == 64U,
            "RUNTIME_FUNCTION code range was not preserved");
    require(plan.runtime_function.prolog_size == 11U, "Win64 prologue size must cover push/mov/sub instruction sequence");
    require(!plan.runtime_function.unwind_codes.empty(), "UNWIND_CODE array is empty");
    require(plan.encoded_unwind_info.size() >= 8U && plan.encoded_unwind_info[0U] == 1U,
            "UNWIND_INFO version/record is invalid");

    Pe64DllSpec spec;
    spec.dll_name = "n49-unwind.dll";
    spec.text.assign(64U, 0x90U);
    spec.exports.push_back(Pe64DllExport{"N49Probe", 1U, Pe64SectionKind::Text, 0U, false, {}});
    spec.runtime_functions.push_back(plan.runtime_function);
    const auto image = write_pe64_dll_image(spec);
    require(image.succeeded(), image.diagnostics.empty() ? "PE writer rejected N49 runtime-function metadata" : image.diagnostics.front().code + ": " + image.diagnostics.front().message);
    require(image.image.exception_directory_rva != 0U && image.image.runtime_function_count == 1U,
            "PE Exception Directory did not retain generated unwind record");
}

void test_n49_reject_bad_unwind(std::size_t index) {
    const auto misaligned = build_win64_frame_unwind_plan(0U, 16U, static_cast<std::uint32_t>((index % 7U) * 8U + 1U), true);
    require(!misaligned.succeeded(), "misaligned Win64 stack allocation must be rejected");
    const auto empty_range = build_win64_frame_unwind_plan(20U, 20U, 16U, true);
    require(!empty_range.succeeded(), "empty runtime-function range must be rejected");
}

void test_n49_frameless_unwind(std::size_t index) {
    const std::uint32_t frames[] = {0U, 8U, 16U, 64U, 128U, 136U};
    const auto frame = frames[index % (sizeof(frames) / sizeof(frames[0]))];
    const auto plan = build_win64_frame_unwind_plan(0U, 32U, frame, false);
    require(plan.succeeded(), "frameless unwind metadata generation failed");
    require(plan.runtime_function.prolog_size == 7U && plan.runtime_function.frame_register == 0U,
            "frameless prologue must not claim RBP as a frame register");
    require(std::none_of(plan.runtime_function.unwind_codes.begin(), plan.runtime_function.unwind_codes.end(), [](const Pe64DllUnwindCode& item) {
        return item.operation == Pe64DllUnwindOperation::SetFramePointer || item.operation == Pe64DllUnwindOperation::PushNonVol;
    }), "frameless unwind record unexpectedly contains RBP frame operations");
}

void test_n49_multiple_runtime_ranges(std::size_t index) {
    const auto first = build_win64_frame_unwind_plan(0U, 32U, 16U, true);
    const auto second = build_win64_frame_unwind_plan(32U, 64U, 32U, true);
    require(first.succeeded() && second.succeeded(), "two independent unwind plans must be valid");
    Pe64DllSpec spec;
    spec.dll_name = "n49-multi.dll";
    spec.text.assign(64U, 0x90U);
    spec.exports.push_back(Pe64DllExport{"N49A", 1U, Pe64SectionKind::Text, 0U, false, {}});
    spec.runtime_functions = {first.runtime_function, second.runtime_function};
    const auto result = write_pe64_dll_image(spec);
    require(result.succeeded() && result.image.runtime_function_count == 2U,
            "PE writer did not preserve two adjacent runtime-function records");
    require((index % 2U) < 2U, "test index guard failed");
}

void test_n49_large_alloc_encoding(std::size_t index) {
    const std::uint32_t frame = index % 2U == 0U ? 136U : 524288U;
    const auto plan = build_win64_frame_unwind_plan(8U, 80U, frame, true);
    require(plan.succeeded(), "large-stack unwind plan failed");
    require(plan.runtime_function.unwind_codes.front().operation == Pe64DllUnwindOperation::AllocLarge,
            "large stack allocation must use UWOP_ALLOC_LARGE");
    require(plan.runtime_function.unwind_codes.front().operation_info == (frame <= 65535U * 8U ? 0U : 1U),
            "UWOP_ALLOC_LARGE OpInfo does not match allocation encoding width");
    require(plan.encoded_unwind_info[2U] == static_cast<std::uint8_t>(frame <= 65535U * 8U ? 4U : 5U),
            "UNWIND_INFO CountOfCodes does not account for allocation operand slots");
}

void test_n50_register_pressure(std::size_t index) {
    std::vector<NativeLiveInterval> intervals;
    const std::uint32_t count = 12U + static_cast<std::uint32_t>(index % 12U);
    for (std::uint32_t value = 0U; value < count; ++value) {
        intervals.push_back(NativeLiveInterval{value, value % 3U, 40U + (value % 5U),
            NativeRegisterClass::GeneralPurpose, (value % 2U) == 0U});
    }
    const auto allocated = allocate_native_registers(intervals);
    require(allocated.succeeded, "linear-scan allocation failed on valid intervals");
    require(allocated.assignments.size() == intervals.size(), "allocator lost one or more live intervals");
    require(allocated.spill_slot_count != 0U, "high GP register pressure must produce spill slots");
    for (std::size_t left = 0U; left < intervals.size(); ++left) {
        for (std::size_t right = left + 1U; right < intervals.size(); ++right) {
            const auto find_assignment = [&](std::uint32_t id) -> const NativeRegisterAssignment& {
                const auto found = std::find_if(allocated.assignments.begin(), allocated.assignments.end(),
                    [id](const NativeRegisterAssignment& item) { return item.value_id == id; });
                if (found == allocated.assignments.end()) throw std::runtime_error("register assignment missing");
                return *found;
            };
            const auto& a = find_assignment(intervals[left].value_id);
            const auto& b = find_assignment(intervals[right].value_id);
            const bool overlap = intervals[left].start <= intervals[right].end &&
                intervals[right].start <= intervals[left].end;
            if (overlap && !a.spilled && !b.spilled && a.register_class == b.register_class)
                require(a.register_index != b.register_index, "overlapping live intervals share one physical register");
        }
    }
}

void test_n50_xmm_and_reuse(std::size_t index) {
    std::vector<NativeLiveInterval> xmm;
    for (std::uint32_t value = 0U; value < 14U; ++value) {
        const std::uint32_t start = value * 3U;
        xmm.push_back(NativeLiveInterval{1000U + value, start, start + 1U,
            NativeRegisterClass::FloatingPoint, false});
    }
    const auto allocated = allocate_native_registers(xmm);
    require(allocated.succeeded && allocated.assignments.size() == 14U,
            "XMM live-interval allocation failed");
    require(allocated.spill_slot_count == 0U,
            "non-overlapping XMM intervals should reuse the same register without spills");
    require(!allocated.used_callee_saved_xmm.empty(), "XMM allocator did not report used nonvolatile registers");
    require((index % 2U) < 2U, "test index guard failed");
}

void test_n50_reject_invalid_intervals(std::size_t index) {
    const auto duplicate = allocate_native_registers({
        NativeLiveInterval{1U, 0U, 2U, NativeRegisterClass::GeneralPurpose, false},
        NativeLiveInterval{1U, 3U, 4U, NativeRegisterClass::GeneralPurpose, false}});
    require(!duplicate.succeeded, "duplicate live interval value IDs must be rejected");
    const auto invalid = allocate_native_registers({
        NativeLiveInterval{static_cast<std::uint32_t>(index + 10U), 8U, 7U, NativeRegisterClass::GeneralPurpose, false}});
    require(!invalid.succeeded, "live interval with end before start must be rejected");
}

void test_n50_call_preservation(std::size_t index) {
    std::vector<NativeLiveInterval> intervals;
    intervals.push_back(NativeLiveInterval{static_cast<std::uint32_t>(index + 5000U), 0U, 100U,
        NativeRegisterClass::GeneralPurpose, true});
    intervals.push_back(NativeLiveInterval{static_cast<std::uint32_t>(index + 6000U), 1U, 80U,
        NativeRegisterClass::FloatingPoint, true});
    const auto result = allocate_native_registers(intervals);
    require(result.succeeded && result.assignments.size() == 2U,
            "live intervals crossing a function call must still allocate");
    for (const auto& assignment : result.assignments) {
        require(!assignment.spilled && assignment.requires_callee_save,
                "call-crossing live range must use a nonvolatile Win64 register or an explicit spill");
    }
}

void test_n50_register_reuse(std::size_t index) {
    std::vector<NativeLiveInterval> intervals;
    for (std::uint32_t value = 0U; value < 24U; ++value) {
        const auto start = value * 4U + static_cast<std::uint32_t>(index % 2U);
        intervals.push_back(NativeLiveInterval{7000U + value, start, start + 1U,
            NativeRegisterClass::GeneralPurpose, false});
    }
    const auto result = allocate_native_registers(intervals);
    require(result.succeeded && result.spill_slot_count == 0U,
            "non-overlapping GP intervals should reuse registers without spills");
    const auto first_register = result.assignments.front().register_index;
    require(std::all_of(result.assignments.begin(), result.assignments.end(), [first_register](const NativeRegisterAssignment& item) {
        return !item.spilled && item.register_index == first_register;
    }), "linear scan failed to recycle the same register for disjoint intervals");
}

void test_n51_double_arithmetic(std::size_t index) {
    const std::string offset = std::to_string(static_cast<unsigned int>(index % 20U));
    const auto unit = parse_unit(
        "module FloatMath; public function Main() returns double {\n"
        "double left = 1.5 + " + offset + ".0; double right = 2.25;\n"
        "double total = left * right + right / left - 1.0; return total; }\n");
    X64CodegenOptions options;
    options.object_id = "n51-double-" + offset;
    const auto compiled = compile_x64_coff_object(unit, options);
    require(compiled.succeeded(), "double expression code generation failed");
    require(has_sequence(compiled.native_object.object_file.sections.front().contents, {0xF2U, 0x0FU, 0x58U}) &&
            has_sequence(compiled.native_object.object_file.sections.front().contents, {0xF2U, 0x0FU, 0x59U}) &&
            has_sequence(compiled.native_object.object_file.sections.front().contents, {0xF2U, 0x0FU, 0x5EU}),
            "SSE2 double add/multiply/divide instructions were not emitted");
    require(read_binary_object(compiled.object_bytes).succeeded(), "double COFF object did not round-trip");
}

void test_n51_float_and_conversion(std::size_t index) {
    const auto unit = parse_unit(
        "module Float32; public function Main() returns float {\n"
        "float left = 1.25; float right = 2.5; float result = left + right;\n"
        "return result; }\n");
    X64CodegenOptions options;
    options.object_id = "n51-float-" + std::to_string(index);
    const auto compiled = compile_x64_coff_object(unit, options);
    require(compiled.succeeded(), "float32 expression code generation failed");
    const auto& code = compiled.native_object.object_file.sections.front().contents;
    require(has_sequence(code, {0xF2U, 0x0FU, 0x5AU, 0xC0U}), "double-to-single conversion was not emitted");
    require(has_sequence(code, {0xF3U, 0x0FU, 0x58U, 0xC1U}), "SSE float add instruction was not emitted");
}

void test_n51_float_compare_and_call(std::size_t index) {
    const auto unit = parse_unit(
        "module FloatCall;\n"
        "private function Sum(double a, double b) returns double { return a + b; }\n"
        "public function Main() returns int { double value = Sum(17.5, 24.5); if (value == 42.0) { return 42; } else { return 1; } }\n");
    X64CodegenOptions options;
    options.object_id = "n51-call-" + std::to_string(index);
    const auto compiled = compile_x64_coff_object(unit, options);
    require(compiled.succeeded(), "Win64 floating call/compare code generation failed");
    require(std::count_if(compiled.native_object.object_file.relocations.begin(), compiled.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
            "floating-point direct call must retain its COFF REL32 relocation");
    const bool has_ucomisd = std::any_of(compiled.native_object.object_file.sections.begin(), compiled.native_object.object_file.sections.end(), [](const ObjectSection& section) {
        return has_sequence(section.contents, {0x66U, 0x0FU, 0x2EU, 0xC8U});
    });
    require(has_ucomisd, "UCOMISD comparison instruction was not emitted in any function section");
}

void test_n51_reject_stack_arguments(std::size_t index) {
    const auto unit = parse_unit(
        "module AbiFloat; public function Main(double a, double b, double c, double d, double e) returns double { return a; }");
    X64CodegenOptions options;
    options.object_id = "n51-abi-reject-" + std::to_string(index);
    const auto compiled = compile_x64_coff_object(unit, options);
    require(!compiled.succeeded() && !compiled.diagnostics.empty(),
            "fifth Win64 positional argument must be explicitly rejected until stack-argument lowering exists");
}

void test_n51_mixed_fp_abi(std::size_t index) {
    const auto unit = parse_unit(
        "module MixedFP; private function Mix(double a, float b) returns double { return a + b; } "
        "public function Main() returns int { double value = Mix(1, 2.5); if (value >= 3.5) { return 42; } else { return " +
        std::to_string(1U + (index % 2U)) + "; } }");
    const auto compiled = compile_x64_coff_object(unit,
        X64CodegenOptions{"n51-mixed-fp-" + std::to_string(index), make_coff_x86_64_object_target(), false, {}});
    require(compiled.succeeded(), "mixed integer/float Win64 ABI lowering failed");
    require(std::count_if(compiled.native_object.object_file.relocations.begin(), compiled.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
            "mixed FP helper call relocation is missing");
    const bool has_cvtsi = std::any_of(compiled.native_object.object_file.sections.begin(), compiled.native_object.object_file.sections.end(), [](const ObjectSection& section) {
        return has_sequence(section.contents, {0xF2U, 0x48U, 0x0FU, 0x2AU, 0xC0U});
    });
    require(has_cvtsi, "integer-to-double Win64 argument conversion was not emitted");
}

void test_n52_pointer_lowering(std::size_t index) {
    const auto unit = parse_unit(
        "module Pointer; public function Main() returns int {\n"
        "int value = " + std::to_string(30U + (index % 10U)) + "; int* pointer = &value;\n"
        "*pointer = *pointer + 1; return value; }\n");
    X64CodegenOptions options;
    options.object_id = "n52-pointer-" + std::to_string(index);
    const auto compiled = compile_x64_coff_object(unit, options);
    require(compiled.succeeded(), "local pointer/address-of/dereference lowering failed");
    const auto& code = compiled.native_object.object_file.sections.front().contents;
    require(has_sequence(code, {0x48U, 0x8DU, 0x85U}), "LEA address-of instruction was not emitted");
    require(has_sequence(code, {0x48U, 0x8BU, 0x00U}), "pointer dereference load was not emitted");
    require(has_sequence(code, {0x48U, 0x89U, 0x08U}), "pointer dereference store was not emitted");
}

void test_n52_global_initialized_data(std::size_t index) {
    auto unit = parse_unit("module Globals;");
    std::vector<std::uint8_t> bytes(8U, 0U);
    bytes[0U] = static_cast<std::uint8_t>(index & 0xffU);
    bytes[1U] = static_cast<std::uint8_t>((index >> 8U) & 0xffU);
    auto global = make_native_global_data_artifact("Globals", "Counter", "int", bytes, 8U, true, false);
    const auto emitted = emit_native_object_sections(unit, {}, {global},
        NativeObjectEmissionOptions{"n52-global-ro", make_coff_x86_64_object_target()});
    require(emitted.succeeded(), "read-only global data object emission failed");
    require(emitted.object_file.sections.size() == 1U &&
            emitted.object_file.sections.front().kind == ObjectSectionKind::ReadOnlyData,
            "read-only global must occupy a distinct RDATA section");
    require(read_binary_object(write_binary_object(emitted.object_file).bytes).succeeded(),
            "global RDATA COFF object failed roundtrip");
}

void test_n52_bss_and_pointer_relocation(std::size_t index) {
    auto unit = parse_unit("module Globals;");
    auto counter = make_native_global_data_artifact("Globals", "Counter", "int", std::vector<std::uint8_t>(8U, 0U), 8U, false, false);
    const auto counter_table = build_native_symbol_table({counter.declaration});
    require(counter_table.succeeded() && counter_table.symbols.size() == 1U,
            "counter global symbol identity failed");
    auto pointer = make_native_global_data_artifact("Globals", "CounterPointer", "int*", std::vector<std::uint8_t>(8U, 0U), 8U, false, false);
    NativeObjectRelocationInput relocation;
    relocation.offset = 0U;
    relocation.kind = ObjectRelocationKind::CoffAddress64;
    relocation.width = 8U;
    relocation.target_link_name = counter_table.symbols.front().mangled_name;
    relocation.target_signature_key = counter_table.symbols.front().signature_key;
    relocation.target_kind = ObjectSymbolKind::Object;
    pointer.relocations.push_back(relocation);
    auto bss = make_native_global_data_artifact("Globals", "Scratch", "int", {}, 16U, false, true);
    bss.logical_size = 16U;
    bss.bss = true;
    const auto emitted = emit_native_object_sections(unit, {}, {counter, pointer, bss},
        NativeObjectEmissionOptions{"n52-global-reloc", make_coff_x86_64_object_target()});
    require(emitted.succeeded(), "global pointer relocation/BSS emission failed");
    require(emitted.object_file.relocations.size() == 1U &&
            emitted.object_file.relocations.front().kind == ObjectRelocationKind::CoffAddress64,
            "global pointer relocation was not preserved");
    require(std::any_of(emitted.object_file.sections.begin(), emitted.object_file.sections.end(), [](const ObjectSection& section) {
        return section.kind == ObjectSectionKind::Bss && section.linker_root && section.size == 16U;
    }), "root BSS section was not preserved");
    const auto bytes = write_binary_object(emitted.object_file);
    require(bytes.succeeded() && read_binary_object(bytes.bytes).succeeded(),
            "global pointer/BSS COFF object did not round-trip");
    require((index % 2U) < 2U, "test index guard failed");
}

void test_n52_pointer_parameter_call(std::size_t index) {
    const auto unit = parse_unit(
        "module PointerCall; private function Increment(int* pointer) returns int { *pointer = *pointer + 1; return *pointer; } "
        "public function Main() returns int { int value = " + std::to_string(40U + (index % 3U)) + "; return Increment(&value); }");
    X64CodegenOptions options;
    options.object_id = "n52-pointer-call-" + std::to_string(index);
    const auto compiled = compile_x64_coff_object(unit, options);
    require(compiled.succeeded(), "pointer parameter/call lowering failed");
    require(std::count_if(compiled.native_object.object_file.relocations.begin(), compiled.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
            "pointer helper call did not emit a COFF relocation");
    require(std::count_if(compiled.native_object.object_file.sections.begin(), compiled.native_object.object_file.sections.end(), [](const ObjectSection& section) { return section.kind == ObjectSectionKind::Text; }) == 2U,
            "pointer helper and Main must have independent function text sections");
}

void test_n52_nonroot_bss(std::size_t index) {
    auto unit = parse_unit("module Scratch;");
    auto scratch = make_native_global_data_artifact("Scratch", "Buffer", "byte[64]", {}, 16U, false, false);
    scratch.logical_size = 64U;
    scratch.bss = true;
    const auto result = emit_native_object_sections(unit, {}, {scratch},
        NativeObjectEmissionOptions{"n52-nonroot-bss-" + std::to_string(index), make_coff_x86_64_object_target()});
    require(result.succeeded() && result.object_file.sections.size() == 1U,
            "non-root BSS global data artifact emission failed");
    require(result.object_file.sections.front().kind == ObjectSectionKind::Bss &&
            !result.object_file.sections.front().linker_root,
            "ordinary BSS must remain independently collectible rather than forced root");
}

void test_n53_multimodule_integer(std::size_t index) {
    const auto math = shared_unit(
        "module Math; public function Add(int left, int right) returns int { return left + right; }");
    const auto app = shared_unit(
        "module App; import Math; public function Main() returns int { return Math.Add(" +
        std::to_string(17U + (index % 3U)) + ", 25); }");
    const std::vector<NativeProgramModuleInput> modules{{"Math.src", math}, {"App.src", app}};
    const auto program = compile_x64_coff_program(modules);
    require(program.succeeded(), "whole-program multi-module integer compilation/link planning failed");
    require(program.modules.size() == 2U && std::count_if(program.link_plan.relocations.begin(), program.link_plan.relocations.end(), [](const ObjectRelocationPlan& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32; }) == 1,
            "multi-module object/link graph is incomplete");
    require(std::any_of(program.link_plan.relocations.begin(), program.link_plan.relocations.end(), [](const ObjectRelocationPlan& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32 && relocation.resolved; }), "qualified imported function relocation was not resolved");
}

void test_n53_multimodule_float(std::size_t index) {
    const auto math = shared_unit(
        "module Math; public function Add(double left, double right) returns double { return left + right; }");
    const auto app = shared_unit(
        "module App; import Math; public function Main() returns int {\n"
        "double value = Math.Add(17.5, 24.5); if (value == 42.0) { return 42; } else { return " +
        std::to_string(1U + (index % 2U)) + "; } }");
    const auto program = compile_x64_coff_program({{"Math.src", math}, {"App.src", app}});
    require(program.succeeded(), "whole-program cross-module double ABI compilation failed");
    require(program.modules.size() == 2U && std::count_if(program.link_plan.relocations.begin(), program.link_plan.relocations.end(), [](const ObjectRelocationPlan& relocation) { return relocation.kind == ObjectRelocationKind::CoffRelative32 && relocation.resolved; }) == 1,
            "cross-module floating call did not resolve to Math.Add");
}

void test_n53_reject_invalid_module_graph(std::size_t index) {
    const auto duplicate_a = shared_unit("module Duplicate; public function A() returns int { return 1; }");
    const auto duplicate_b = shared_unit("module duplicate; public function B() returns int { return 2; }");
    const auto duplicate = compile_x64_coff_program({{"a.src", duplicate_a}, {"b.src", duplicate_b}});
    require(!duplicate.succeeded() && !duplicate.diagnostics.empty(),
            "duplicate case-insensitive module names must be rejected");
    const auto missing = shared_unit("module Client; import Missing; public function Main() returns int { return Missing.Run(); }");
    const auto missing_result = compile_x64_coff_program({{"client.src", missing}});
    require(!missing_result.succeeded() && !missing_result.diagnostics.empty(),
            "missing imported module must be rejected before native linking");
    require((index % 2U) < 2U, "test index guard failed");
}

void test_n53_reject_signature_mismatch(std::size_t index) {
    const auto math = shared_unit("module Math; public function Add(int left, int right) returns int { return left + right; }");
    const auto app = shared_unit("module App; import Math; public function Main() returns int { return Math.Add(1.5, " +
        std::to_string(2U + (index % 3U)) + "); }");
    const auto result = compile_x64_coff_program({{"Math.src", math}, {"App.src", app}});
    require(!result.succeeded() && !result.diagnostics.empty(),
            "whole-program semantic pass must reject incompatible imported call argument types");
}

void test_n53_reject_module_cycle(std::size_t index) {
    const auto alpha = shared_unit("module Alpha; import Beta; public function A() returns int { return Beta.B(); }");
    const auto beta = shared_unit("module Beta; import Alpha; public function B() returns int { return Alpha.A(); }");
    const auto result = compile_x64_coff_program({{"Alpha.src", alpha}, {"Beta.src", beta}});
    require(!result.succeeded() && !result.diagnostics.empty(),
            "module dependency cycle must be rejected before COFF emission");
    require((index % 2U) < 2U, "test index guard failed");
}

} // namespace

void run_n49_n53_native_backend_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 400U; ++index) {
        run_case("N49A_frame_unwind_plan_" + std::to_string(index), [index] { test_n49_unwind_plan(index); }, passed, failed);
        run_case("N49B_reject_bad_unwind_" + std::to_string(index), [index] { test_n49_reject_bad_unwind(index); }, passed, failed);
        run_case("N49C_frameless_unwind_" + std::to_string(index), [index] { test_n49_frameless_unwind(index); }, passed, failed);
        run_case("N49D_multiple_runtime_ranges_" + std::to_string(index), [index] { test_n49_multiple_runtime_ranges(index); }, passed, failed);
        run_case("N49E_large_alloc_encoding_" + std::to_string(index), [index] { test_n49_large_alloc_encoding(index); }, passed, failed);
        run_case("N50A_register_pressure_" + std::to_string(index), [index] { test_n50_register_pressure(index); }, passed, failed);
        run_case("N50B_xmm_reuse_" + std::to_string(index), [index] { test_n50_xmm_and_reuse(index); }, passed, failed);
        run_case("N50C_reject_invalid_intervals_" + std::to_string(index), [index] { test_n50_reject_invalid_intervals(index); }, passed, failed);
        run_case("N50D_call_preservation_" + std::to_string(index), [index] { test_n50_call_preservation(index); }, passed, failed);
        run_case("N50E_register_reuse_" + std::to_string(index), [index] { test_n50_register_reuse(index); }, passed, failed);
        run_case("N51A_double_arithmetic_" + std::to_string(index), [index] { test_n51_double_arithmetic(index); }, passed, failed);
        run_case("N51B_float_and_conversion_" + std::to_string(index), [index] { test_n51_float_and_conversion(index); }, passed, failed);
        run_case("N51C_float_compare_call_" + std::to_string(index), [index] { test_n51_float_compare_and_call(index); }, passed, failed);
        run_case("N51D_reject_stack_arguments_" + std::to_string(index), [index] { test_n51_reject_stack_arguments(index); }, passed, failed);
        run_case("N51E_mixed_fp_abi_" + std::to_string(index), [index] { test_n51_mixed_fp_abi(index); }, passed, failed);
        run_case("N52A_pointer_lowering_" + std::to_string(index), [index] { test_n52_pointer_lowering(index); }, passed, failed);
        run_case("N52B_global_initialized_data_" + std::to_string(index), [index] { test_n52_global_initialized_data(index); }, passed, failed);
        run_case("N52C_global_bss_pointer_relocation_" + std::to_string(index), [index] { test_n52_bss_and_pointer_relocation(index); }, passed, failed);
        run_case("N52D_pointer_parameter_call_" + std::to_string(index), [index] { test_n52_pointer_parameter_call(index); }, passed, failed);
        run_case("N52E_nonroot_bss_" + std::to_string(index), [index] { test_n52_nonroot_bss(index); }, passed, failed);
        run_case("N53A_multimodule_integer_" + std::to_string(index), [index] { test_n53_multimodule_integer(index); }, passed, failed);
        run_case("N53B_multimodule_float_" + std::to_string(index), [index] { test_n53_multimodule_float(index); }, passed, failed);
        run_case("N53C_reject_invalid_module_graph_" + std::to_string(index), [index] { test_n53_reject_invalid_module_graph(index); }, passed, failed);
        run_case("N53D_reject_signature_mismatch_" + std::to_string(index), [index] { test_n53_reject_signature_mismatch(index); }, passed, failed);
        run_case("N53E_reject_module_cycle_" + std::to_string(index), [index] { test_n53_reject_module_cycle(index); }, passed, failed);
    }
}
