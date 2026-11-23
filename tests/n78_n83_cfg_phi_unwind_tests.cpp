#include "xfrontend/native_cfg_bridge.hpp"
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

NativeRegisterFunctionIR phi_diamond() {
    NativeRegisterFunctionIR function;
    function.values = {{1U, NativeRegisterClass::GeneralPurpose},
                       {2U, NativeRegisterClass::GeneralPurpose},
                       {3U, NativeRegisterClass::GeneralPurpose},
                       {4U, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {
        {0U, {}, {1U, 2U}, {}},
        {1U, {}, {3U}, {}},
        {2U, {}, {3U}, {}},
        {3U, {}, {}, {}}
    };
    return function;
}

NativeRegisterLocation reg(std::uint8_t index) {
    NativeRegisterLocation value;
    value.kind = NativeRegisterLocationKind::Register;
    value.register_index = index;
    return value;
}

NativeRegisterLocation spill(std::uint32_t index) {
    NativeRegisterLocation value;
    value.kind = NativeRegisterLocationKind::SpillSlot;
    value.spill_slot = index;
    return value;
}

NativeRegisterLocation temp(std::uint32_t index) {
    NativeRegisterLocation value;
    value.kind = NativeRegisterLocationKind::Temporary;
    value.spill_slot = index;
    return value;
}

std::string cfg_source(std::size_t index) {
    const auto bump = 1U + static_cast<unsigned int>(index % 11U);
    return "module N78; "
           "function Touch(int value) returns int { return value + 1; } "
           "function Main(bool flag, int seed, double scale) returns int { "
           "int result = seed; double fraction = scale; "
           "if (flag) { result = Touch(result) + " + std::to_string(bump) + "; } "
           "else { result = result + 2; } "
           "while (flag) { fraction = fraction + 0.5; result = result + 1; break; } "
           "return result; }";
}

void test_n78_real_frontend_cfg_bridge(std::size_t index) {
    const auto parsed = parse_source(cfg_source(index));
    require(parsed.succeeded(), "N78 CFG source failed to parse");
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded && lowered.functions.size() == 2U,
            "N78 did not lower both frontend functions into native register IR");
    const auto& main_function = lowered.functions.back();
    require(main_function.register_ir.blocks.size() >= 6U,
            "N78 if/else and loop did not produce a multi-block native CFG");
    bool saw_fp_value = false;
    bool saw_call = false;
    bool saw_back_edge = false;
    for (const auto& value : main_function.register_ir.values) {
        saw_fp_value = saw_fp_value || value.register_class == NativeRegisterClass::FloatingPoint;
    }
    for (const auto& block : main_function.register_ir.blocks) {
        for (const auto successor : block.successors) {
            if (successor <= block.id) saw_back_edge = true;
        }
        for (const auto& instruction : block.instructions) saw_call = saw_call || instruction.is_call;
    }
    require(saw_fp_value, "N78 lost floating-point local/parameter class metadata");
    require(saw_call, "N78 lost the call-clobber operation from frontend CFG");
    require(saw_back_edge, "N78 loop back-edge was not preserved");
    const auto liveness = compute_native_register_liveness(main_function.register_ir);
    require(liveness.succeeded && liveness.converged,
            "N78 multi-block CFG liveness did not reach a fixed point");
}

void test_n79_phi_incoming_validation(std::size_t index) {
    (void)index;
    const auto function = phi_diamond();
    const std::vector<NativeRegisterPhiNode> valid_phis{{3U, 4U, {{1U, 1U}, {2U, 2U}}}};
    const auto valid = validate_native_register_phi_nodes(function, valid_phis);
    require(valid.succeeded, "N79 valid diamond Phi was rejected");

    const std::vector<NativeRegisterPhiNode> missing_edge{{3U, 4U, {{1U, 1U}}}};
    const auto missing = validate_native_register_phi_nodes(function, missing_edge);
    require(!missing.succeeded && !missing.diagnostics.empty(),
            "N79 Phi with a missing predecessor was accepted");

    const std::vector<NativeRegisterPhiNode> foreign_edge{{3U, 4U, {{1U, 1U}, {0U, 2U}}}};
    const auto foreign = validate_native_register_phi_nodes(function, foreign_edge);
    require(!foreign.succeeded && !foreign.diagnostics.empty(),
            "N79 Phi with a non-predecessor edge was accepted");

    auto mixed_classes = function;
    mixed_classes.values.push_back({5U, NativeRegisterClass::FloatingPoint});
    const std::vector<NativeRegisterPhiNode> wrong_class{{3U, 4U, {{1U, 1U}, {2U, 5U}}}};
    const auto mismatch = validate_native_register_phi_nodes(mixed_classes, wrong_class);
    require(!mismatch.succeeded && !mismatch.diagnostics.empty(),
            "N79 Phi with a register-class mismatch was accepted");
}

void test_n80_parallel_copy_schedules_and_rejections(std::size_t index) {
    const auto base = static_cast<std::uint8_t>(index % 8U);
    const std::vector<NativeRegisterParallelCopy> acyclic{
        {1U, reg(base), reg(static_cast<std::uint8_t>(base + 1U))},
        {2U, reg(static_cast<std::uint8_t>(base + 1U)), spill(static_cast<std::uint32_t>(index % 5U))}
    };
    const auto straight = resolve_native_register_parallel_copies(acyclic, temp(900U));
    require(straight.succeeded && straight.operations.size() == 2U &&
            straight.cycle_temporaries_used == 0U,
            "N80 acyclic parallel copy did not resolve without a cycle temporary");

    const auto a = static_cast<std::uint8_t>(index % 10U);
    const auto b = static_cast<std::uint8_t>((a + 1U) % 10U);
    const std::vector<NativeRegisterParallelCopy> cycle{
        {1U, reg(a), reg(b)}, {2U, reg(b), reg(a)}
    };
    const auto cyclic = resolve_native_register_parallel_copies(cycle, temp(1000U));
    require(cyclic.succeeded && cyclic.cycle_temporaries_used == 1U &&
            cyclic.operations.size() == 3U,
            "N80 two-register cycle did not resolve through one temporary");

    const std::vector<NativeRegisterParallelCopy> duplicate{
        {1U, reg(1U), reg(3U)}, {2U, reg(2U), reg(3U)}
    };
    const auto rejected = resolve_native_register_parallel_copies(duplicate, temp(1200U));
    require(!rejected.succeeded && !rejected.diagnostics.empty(),
            "N80 duplicate parallel-copy destination was accepted");
}

void test_n81_flow_locals_and_parameters_are_allocated(std::size_t index) {
    const auto parsed = parse_source(cfg_source(index));
    require(parsed.succeeded(), "N81 source failed to parse");
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded, "N81 frontend CFG lowering failed");
    const auto& function = lowered.functions.back();
    const auto result = analyze_and_allocate_native_flow_function(function);
    require(result.succeeded, "N81 flow-derived register allocation failed");
    require(!function.bindings.empty() && !result.intervals.intervals.empty(),
            "N81 did not create intervals for persistent locals/parameters");
    require(result.liveness.blocks.size() == function.register_ir.blocks.size(),
            "N81 liveness result does not cover every CFG block");
}

void test_n82_frame_plan_from_flow_allocation(std::size_t index) {
    const auto parsed = parse_source(cfg_source(index));
    require(parsed.succeeded(), "N82 source failed to parse");
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded, "N82 CFG lowering failed");
    const auto allocation = analyze_and_allocate_native_flow_function(lowered.functions.back());
    require(allocation.succeeded, "N82 allocation planning failed");
    const auto frame = build_native_win64_frame_plan(allocation.allocation,
        static_cast<std::uint32_t>(16U + (index % 8U) * 8U));
    require(frame.succeeded, "N82 Win64 frame planner rejected flow-derived allocation");
    require((frame.frame_size_bytes % 16U) == 0U,
            "N82 resulting Win64 frame is not 16-byte aligned");
}

void test_n83_win64_unwind_encoding_and_rejections(std::size_t index) {
    const std::uint32_t allocation = 160U + static_cast<std::uint32_t>(index % 8U) * 16U;
    const std::vector<Win64PrologueAction> actions{
        {Win64PrologueAction::Kind::PushNonvolatileGpr, 1U, 5U, 0U, 0U},
        {Win64PrologueAction::Kind::SetFramePointer, 4U, 5U, 0U, 0U},
        {Win64PrologueAction::Kind::PushNonvolatileGpr, 5U, 3U, 0U, 0U},
        {Win64PrologueAction::Kind::AllocateStack, 10U, 0U, 0U, allocation},
        {Win64PrologueAction::Kind::SaveNonvolatileXmm, 15U, 6U, 32U, 0U},
        {Win64PrologueAction::Kind::SaveNonvolatileGpr, 17U, 12U, 48U, 0U}
    };
    const auto result = build_win64_prologue_unwind_plan(0U, 128U, 17U, 5U, 0U, actions);
    require(result.succeeded(), "N83 prologue actions failed to encode as UNWIND_INFO");
    require(result.runtime_function.unwind_codes.size() == actions.size(),
            "N83 did not preserve all prologue unwind actions");
    require(!result.encoded_unwind_info.empty() && result.encoded_unwind_info.front() == 1U,
            "N83 UNWIND_INFO version byte is not version 1");
    require(result.encoded_unwind_info[1] == 17U,
            "N83 encoded prologue length is incorrect");
    require(result.runtime_function.unwind_codes.front().code_offset == 17U &&
            result.runtime_function.unwind_codes.back().code_offset == 1U,
            "N83 unwind opcodes are not sorted in descending CodeOffset order");
    Pe64DllSpec dll_spec;
    dll_spec.dll_name = "N83UnwindSmoke.dll";
    dll_spec.text.assign(128U, 0x90U);
    dll_spec.exports.push_back(Pe64DllExport{"N83Smoke", 1U, Pe64SectionKind::Text, 0U, false, {}});
    dll_spec.runtime_functions.push_back(result.runtime_function);
    const auto dll_image = write_pe64_dll_image(dll_spec);
    if (!dll_image.succeeded() || dll_image.image.runtime_function_count != 1U ||
        dll_image.image.exception_directory_size != 12U ||
        dll_image.image.unwind_info_size < result.encoded_unwind_info.size()) {
        std::string detail = "N83 PE unwind writer rejected metadata";
        if (!dll_image.diagnostics.empty()) detail += ": " + dll_image.diagnostics.front().code + " " + dll_image.diagnostics.front().message;
        detail += " | runtime_functions=" + std::to_string(dll_image.image.runtime_function_count);
        detail += " | exception_size=" + std::to_string(dll_image.image.exception_directory_size);
        detail += " | unwind_size=" + std::to_string(dll_image.image.unwind_info_size);
        detail += " | encoded_size=" + std::to_string(result.encoded_unwind_info.size());
        throw std::runtime_error(detail);
    }

    const std::vector<Win64PrologueAction> bad_alignment{
        {Win64PrologueAction::Kind::AllocateStack, 4U, 0U, 0U,
         static_cast<std::uint32_t>(25U + (index % 5U))}
    };
    const auto rejected_alignment = build_win64_prologue_unwind_plan(0U, 64U, 8U, 0U, 0U, bad_alignment);
    require(!rejected_alignment.succeeded(), "N83 unaligned stack allocation was accepted");

    const std::vector<Win64PrologueAction> bad_xmm{
        {Win64PrologueAction::Kind::SaveNonvolatileXmm, 4U, 5U, 16U, 0U}
    };
    const auto rejected_xmm = build_win64_prologue_unwind_plan(0U, 64U, 8U, 0U, 0U, bad_xmm);
    require(!rejected_xmm.succeeded(), "N83 volatile XMM save was accepted");
}

} // namespace

void run_n78_n83_cfg_phi_unwind_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 2000U; ++index) {
        run_case("N78A_frontend_cfg_register_bridge_" + std::to_string(index),
            [index] { test_n78_real_frontend_cfg_bridge(index); }, passed, failed);
        run_case("N79A_phi_predecessor_validation_" + std::to_string(index),
            [index] { test_n79_phi_incoming_validation(index); }, passed, failed);
        run_case("N80A_parallel_copy_resolution_" + std::to_string(index),
            [index] { test_n80_parallel_copy_schedules_and_rejections(index); }, passed, failed);
        run_case("N81A_cfg_local_parameter_allocation_" + std::to_string(index),
            [index] { test_n81_flow_locals_and_parameters_are_allocated(index); }, passed, failed);
        run_case("N82A_flow_win64_frame_plan_" + std::to_string(index),
            [index] { test_n82_frame_plan_from_flow_allocation(index); }, passed, failed);
        run_case("N83A_win64_unwind_plan_" + std::to_string(index),
            [index] { test_n83_win64_unwind_encoding_and_rejections(index); }, passed, failed);
    }
}
