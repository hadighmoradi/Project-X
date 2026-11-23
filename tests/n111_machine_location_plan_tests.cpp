#include "test_report.hpp"

#include "xfrontend/x64_location_transfer.hpp"

#include <cstdint>
#include <functional>
#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace xfrontend;

void require(bool condition, const char* message) {
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

NativeRegisterFlowFunction make_flow_fixture(bool overlapping = true,
                                             bool include_float = true,
                                             std::size_t value_count = 3U) {
    NativeRegisterFlowFunction flow;
    flow.function_name = "n111_machine_location_fixture";
    NativeRegisterBasicBlock block;
    block.id = 0U;
    for (std::size_t index = 0U; index < value_count; ++index) {
        const auto id = static_cast<std::uint32_t>(index + 1U);
        const auto register_class = include_float && index == 2U
            ? NativeRegisterClass::FloatingPoint : NativeRegisterClass::GeneralPurpose;
        flow.register_ir.values.push_back(NativeRegisterValueInfo{id, register_class});
        NativeRegisterFlowValueBinding binding;
        binding.flow_variable_id = static_cast<FlowVariableId>(id);
        binding.native_value_id = id;
        binding.name = "v" + std::to_string(id);
        binding.type_name = register_class == NativeRegisterClass::FloatingPoint ? "double" : "int";
        binding.declaration_span.start.byte_offset = index * 4U;
        binding.declaration_span.end.byte_offset = index * 4U + 2U;
        flow.bindings.push_back(std::move(binding));
    }
    if (overlapping) {
        for (std::size_t index = 0U; index < value_count; ++index) {
            NativeRegisterInstruction definition;
            definition.definitions.push_back(static_cast<std::uint32_t>(index + 1U));
            block.instructions.push_back(std::move(definition));
        }
        NativeRegisterInstruction combined_use;
        for (std::size_t index = 0U; index < value_count; ++index)
            combined_use.uses.push_back(static_cast<std::uint32_t>(index + 1U));
        block.instructions.push_back(std::move(combined_use));
    } else {
        for (std::size_t index = 0U; index < value_count; ++index) {
            NativeRegisterInstruction definition;
            definition.definitions.push_back(static_cast<std::uint32_t>(index + 1U));
            block.instructions.push_back(std::move(definition));
            NativeRegisterInstruction use;
            use.uses.push_back(static_cast<std::uint32_t>(index + 1U));
            block.instructions.push_back(std::move(use));
        }
    }
    flow.register_ir.entry_block = 0U;
    flow.register_ir.blocks.push_back(std::move(block));
    return flow;
}

NativeRegisterLivenessResult liveness_for(const NativeRegisterFlowFunction& flow) {
    return compute_native_register_liveness(flow.register_ir);
}

NativeRegisterAssignment assignment(std::uint32_t value_id, NativeRegisterClass register_class,
                                    std::uint8_t register_index, bool spilled = false,
                                    bool requires_callee_save = true) {
    NativeRegisterAssignment result;
    result.value_id = value_id;
    result.register_class = register_class;
    result.spilled = spilled;
    result.register_index = register_index;
    result.requires_callee_save = requires_callee_save;
    return result;
}

NativeRegisterAllocationResult allocation(std::initializer_list<NativeRegisterAssignment> assignments) {
    NativeRegisterAllocationResult result;
    result.succeeded = true;
    result.assignments.assign(assignments.begin(), assignments.end());
    return result;
}

void test_home_fallback_is_authoritative_for_unassigned_values() {
    const auto flow = make_flow_fixture();
    const auto live = liveness_for(flow);
    const auto plan = build_native_x64_machine_location_plan(flow, live, allocation({}), {}, {}, {});
    require(plan.succeeded && plan.values.size() == 3U &&
            plan.register_resident_values == 0U && plan.home_resident_values == 3U,
        "N111 did not create a complete HomeSlot fallback for every Flow binding");
    for (const auto& item : plan.values) {
        require(item.second.active_location.kind == NativeRegisterLocationKind::HomeSlot &&
                item.second.canonical_home.kind == NativeRegisterLocationKind::HomeSlot &&
                item.second.canonical_home.spill_slot == InvalidNativeX64HomeSlot,
            "N111 home fallback does not retain the unresolved canonical-home sentinel");
    }
}

void test_gp_persistent_location_is_selected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 3U)}), {}, {}, {});
    require(plan.succeeded && plan.values.at(1U).active_location.kind == NativeRegisterLocationKind::Register &&
            plan.values.at(1U).active_location.register_index == 3U &&
            plan.values.at(2U).active_location.kind == NativeRegisterLocationKind::HomeSlot &&
            plan.register_resident_values == 1U && plan.home_resident_values == 1U &&
            plan.used_callee_saved_gp == std::vector<std::uint8_t>{3U},
        "N111 did not select the assigned Win64 nonvolatile GP location");
}

void test_xmm_persistent_location_is_selected() {
    const auto flow = make_flow_fixture(true, true, 3U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(3U, NativeRegisterClass::FloatingPoint, 6U)}), {}, {}, {});
    require(plan.succeeded && plan.values.at(3U).active_location.kind == NativeRegisterLocationKind::Register &&
            plan.values.at(3U).active_location.register_index == 6U &&
            plan.used_callee_saved_xmm == std::vector<std::uint8_t>{6U},
        "N111 did not select the assigned Win64 nonvolatile XMM location");
}

void test_spilled_value_uses_canonical_home_not_allocator_spill_as_active_location() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 0U, true, false)}), {}, {}, {});
    require(plan.succeeded && plan.values.at(1U).active_location.kind == NativeRegisterLocationKind::HomeSlot &&
            plan.home_resident_values == 2U,
        "N111 did not keep a spilled persistent source value on its canonical local home");
}

void test_address_taken_value_must_remain_memory_resident() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({}), {1U}, {}, {});
    require(plan.succeeded && plan.values.at(1U).address_taken &&
            plan.values.at(1U).active_location.kind == NativeRegisterLocationKind::HomeSlot &&
            plan.address_taken_values == 1U,
        "N111 did not keep an address-taken binding in its canonical memory home");
}

void test_address_taken_register_assignment_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 3U)}), {1U}, {}, {});
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N111ME000C",
        "N111 accepted a register assignment for an address-taken binding");
}

void test_volatile_gp_assignment_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 8U, false, false)}), {}, {}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME0007",
        "N111 accepted a volatile GP assignment without a call-safe plan");
}

void test_volatile_xmm_assignment_is_rejected() {
    const auto flow = make_flow_fixture(true, true, 3U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(3U, NativeRegisterClass::FloatingPoint, 5U, false, false)}), {}, {}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME0007",
        "N111 accepted a volatile XMM assignment without a call-safe plan");
}

void test_wrong_register_class_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::FloatingPoint, 6U)}), {}, {}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME0006",
        "N111 accepted a persistent assignment with the wrong register class");
}

void test_expression_reserved_gp_register_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 6U)}), {}, {6U}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME0009",
        "N111 allowed a persistent GP value to alias an expression-temporary register");
}

void test_expression_reserved_xmm_register_is_rejected() {
    const auto flow = make_flow_fixture(true, true, 3U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(3U, NativeRegisterClass::FloatingPoint, 9U)}), {}, {}, {9U});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME000B",
        "N111 allowed a persistent XMM value to alias an expression-temporary register");
}

void test_interfering_values_cannot_share_gp_register() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 3U),
                    assignment(2U, NativeRegisterClass::GeneralPurpose, 3U)}), {}, {}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME000E",
        "N111 accepted interfering GP values in the same physical register");
}

void test_interfering_values_cannot_share_xmm_register() {
    auto flow = make_flow_fixture(true, true, 3U);
    // Make the first two simultaneously-live values XMM values so this test
    // reaches the interference-alias invariant instead of failing earlier on
    // a GP/XMM class mismatch.
    flow.register_ir.values[1].register_class = NativeRegisterClass::FloatingPoint;
    flow.bindings[1].type_name = "double";
    const auto conflict = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(3U, NativeRegisterClass::FloatingPoint, 6U),
                    assignment(2U, NativeRegisterClass::FloatingPoint, 6U)}), {}, {}, {});
    require(!conflict.succeeded && !conflict.diagnostics.empty() &&
            conflict.diagnostics.front().code == "N111ME000E",
        "N111 accepted interfering XMM values in the same physical register");
}

void test_noninterfering_values_may_reuse_register() {
    const auto flow = make_flow_fixture(false, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 3U),
                    assignment(2U, NativeRegisterClass::GeneralPurpose, 3U)}), {}, {}, {});
    require(plan.succeeded && plan.register_resident_values == 2U &&
            plan.values.at(1U).active_location.register_index == 3U &&
            plan.values.at(2U).active_location.register_index == 3U,
        "N111 did not allow safe register reuse for non-interfering Flow values");
}

void test_unknown_assignment_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(99U, NativeRegisterClass::GeneralPurpose, 3U)}), {}, {}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME0005",
        "N111 accepted an allocation for a value outside the source binding set");
}

void test_duplicate_bindings_are_rejected() {
    auto flow = make_flow_fixture(true, false, 2U);
    flow.bindings.push_back(flow.bindings.front());
    const auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow), allocation({}), {}, {}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME0003",
        "N111 accepted duplicate source binding identities");
}

void test_home_binding_resolves_active_home_location() {
    const auto flow = make_flow_fixture(true, false, 2U);
    auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow), allocation({}), {}, {}, {});
    require(plan.succeeded, "N111 could not build a home-resident plan fixture");
    require(bind_native_x64_machine_location_home(plan, 1U, 5U),
        "N111 could not bind a concrete canonical home slot");
    const auto& value = plan.values.at(1U);
    require(value.canonical_home.kind == NativeRegisterLocationKind::HomeSlot &&
            value.canonical_home.spill_slot == 5U &&
            value.active_location.kind == NativeRegisterLocationKind::HomeSlot &&
            value.active_location.spill_slot == 5U,
        "N111 home-resident value did not use its concrete canonical home as the active location");
}

void test_home_binding_preserves_register_active_location() {
    const auto flow = make_flow_fixture(true, false, 2U);
    auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow),
        allocation({assignment(1U, NativeRegisterClass::GeneralPurpose, 3U)}), {}, {}, {});
    require(plan.succeeded, "N111 could not build a register-resident plan fixture");
    require(bind_native_x64_machine_location_home(plan, 1U, 7U),
        "N111 could not bind the canonical home for a register-resident value");
    const auto& value = plan.values.at(1U);
    require(value.canonical_home.spill_slot == 7U &&
            value.active_location.kind == NativeRegisterLocationKind::Register &&
            value.active_location.register_index == 3U,
        "N111 canonical-home binding overwrote the active persistent register");
}

void test_same_home_binding_is_idempotent() {
    const auto flow = make_flow_fixture(true, false, 2U);
    auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow), allocation({}), {}, {}, {});
    require(plan.succeeded && bind_native_x64_machine_location_home(plan, 1U, 9U) &&
            bind_native_x64_machine_location_home(plan, 1U, 9U) && plan.succeeded,
        "N111 repeated binding to the same physical HomeSlot was not idempotent");
}

void test_conflicting_home_rebinding_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow), allocation({}), {}, {}, {});
    require(plan.succeeded && bind_native_x64_machine_location_home(plan, 1U, 4U),
        "N111 could not establish the initial canonical home");
    require(!bind_native_x64_machine_location_home(plan, 1U, 5U) && !plan.succeeded &&
            plan.diagnostics.back().code == "N111ME0014",
        "N111 accepted a conflicting canonical HomeSlot rebind");
}

void test_unknown_home_binding_value_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow), allocation({}), {}, {}, {});
    require(plan.succeeded, "N111 could not build a home binding fixture");
    require(!bind_native_x64_machine_location_home(plan, 99U, 4U) && !plan.succeeded &&
            plan.diagnostics.back().code == "N111ME0012",
        "N111 accepted a canonical home binding for an unknown Flow value");
}

void test_invalid_home_slot_sentinel_is_rejected() {
    const auto flow = make_flow_fixture(true, false, 2U);
    auto plan = build_native_x64_machine_location_plan(flow, liveness_for(flow), allocation({}), {}, {}, {});
    require(plan.succeeded, "N111 could not build a home binding fixture");
    require(!bind_native_x64_machine_location_home(plan, 1U, InvalidNativeX64HomeSlot) &&
            !plan.succeeded && plan.diagnostics.back().code == "N111ME0011",
        "N111 accepted the reserved invalid HomeSlot sentinel as a physical home");
}

void test_missing_liveness_is_rejected() {
    const auto flow = make_flow_fixture();
    NativeRegisterLivenessResult missing;
    const auto plan = build_native_x64_machine_location_plan(flow, missing, allocation({}), {}, {}, {});
    require(!plan.succeeded && plan.diagnostics.front().code == "N111ME0001",
        "N111 accepted incomplete liveness data");
}

} // namespace

void run_n111_machine_location_plan_tests(int& passed, int& failed) {
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"N111A_home_fallback_is_authoritative", test_home_fallback_is_authoritative_for_unassigned_values},
        {"N111B_gp_persistent_location_selected", test_gp_persistent_location_is_selected},
        {"N111C_xmm_persistent_location_selected", test_xmm_persistent_location_is_selected},
        {"N111D_spilled_value_uses_canonical_home", test_spilled_value_uses_canonical_home_not_allocator_spill_as_active_location},
        {"N111E_address_taken_stays_memory", test_address_taken_value_must_remain_memory_resident},
        {"N111F_address_taken_register_rejected", test_address_taken_register_assignment_is_rejected},
        {"N111G_volatile_gp_rejected", test_volatile_gp_assignment_is_rejected},
        {"N111H_volatile_xmm_rejected", test_volatile_xmm_assignment_is_rejected},
        {"N111I_register_class_mismatch_rejected", test_wrong_register_class_is_rejected},
        {"N111J_expression_reserved_gp_rejected", test_expression_reserved_gp_register_is_rejected},
        {"N111K_expression_reserved_xmm_rejected", test_expression_reserved_xmm_register_is_rejected},
        {"N111L_interfering_gp_alias_rejected", test_interfering_values_cannot_share_gp_register},
        {"N111M_interfering_xmm_alias_rejected", test_interfering_values_cannot_share_xmm_register},
        {"N111N_noninterfering_register_reuse_allowed", test_noninterfering_values_may_reuse_register},
        {"N111O_unknown_assignment_rejected", test_unknown_assignment_is_rejected},
        {"N111P_duplicate_binding_rejected", test_duplicate_bindings_are_rejected},
        {"N111Q_home_binding_resolves_active_location", test_home_binding_resolves_active_home_location},
        {"N111R_home_binding_preserves_register", test_home_binding_preserves_register_active_location},
        {"N111S_same_home_binding_is_idempotent", test_same_home_binding_is_idempotent},
        {"N111T_conflicting_home_rebind_rejected", test_conflicting_home_rebinding_is_rejected},
        {"N111U_unknown_home_binding_rejected", test_unknown_home_binding_value_is_rejected},
        {"N111V_invalid_home_sentinel_rejected", test_invalid_home_slot_sentinel_is_rejected},
        {"N111W_missing_liveness_rejected", test_missing_liveness_is_rejected}
    };
    for (const auto& item : cases) run_case(item.first, item.second, passed, failed);
}
