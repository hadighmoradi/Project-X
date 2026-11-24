// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_report.hpp"

#include "xfrontend/native_register_analysis.hpp"

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

NativeRegisterInstruction flow_instruction(
    NativeRegisterInstructionOriginKind origin, std::uint32_t flow_index) {
    NativeRegisterInstruction instruction;
    instruction.origin_kind = origin;
    instruction.flow_block_id = 0U;
    instruction.flow_operation_index = flow_index;
    instruction.flow_operation_id = (std::uint64_t{1U} << 32U) |
                                    (static_cast<std::uint64_t>(flow_index) + 1U);
    instruction.source_span.start.byte_offset = 10U + 2U * flow_index;
    instruction.source_span.end.byte_offset = 11U + 2U * flow_index;
    return instruction;
}

NativeRegisterFunctionIR make_call_fixture() {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values.push_back(NativeRegisterValueInfo{7U, NativeRegisterClass::GeneralPurpose});
    NativeRegisterBasicBlock block;
    block.id = 0U;

    NativeRegisterInstruction parameter_definition;
    parameter_definition.definitions.push_back(7U);
    block.instructions.push_back(parameter_definition); // Synthetic instruction 0.

    auto first_read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 0U);
    first_read.uses.push_back(7U);
    block.instructions.push_back(first_read); // Native instruction 1, Flow operation 0.

    auto call = flow_instruction(NativeRegisterInstructionOriginKind::FlowCall, 1U);
    call.is_call = true;
    block.instructions.push_back(call); // Native instruction 2, Flow operation 1.

    auto second_read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 2U);
    second_read.uses.push_back(7U);
    block.instructions.push_back(second_read);

    auto return_op = flow_instruction(NativeRegisterInstructionOriginKind::FlowReturn, 3U);
    return_op.uses.push_back(7U);
    block.instructions.push_back(return_op);

    function.blocks.push_back(std::move(block));
    return function;
}

NativeRegisterFunctionIR make_pressure_call_fixture(std::size_t value_count = 7U) {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    NativeRegisterBasicBlock block;
    block.id = 0U;
    for (std::size_t index = 0U; index < value_count; ++index) {
        const auto value_id = static_cast<std::uint32_t>(index + 1U);
        function.values.push_back(NativeRegisterValueInfo{value_id, NativeRegisterClass::GeneralPurpose});
        auto definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare,
                                           static_cast<std::uint32_t>(index));
        definition.definitions.push_back(value_id);
        block.instructions.push_back(std::move(definition));
    }
    auto call = flow_instruction(NativeRegisterInstructionOriginKind::FlowCall,
                                 static_cast<std::uint32_t>(value_count));
    call.is_call = true;
    block.instructions.push_back(call);
    auto combined_read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead,
                                          static_cast<std::uint32_t>(value_count + 1U));
    for (std::size_t index = 0U; index < value_count; ++index)
        combined_read.uses.push_back(static_cast<std::uint32_t>(index + 1U));
    block.instructions.push_back(std::move(combined_read));
    auto return_op = flow_instruction(NativeRegisterInstructionOriginKind::FlowReturn,
                                      static_cast<std::uint32_t>(value_count + 2U));
    block.instructions.push_back(std::move(return_op));
    function.blocks.push_back(std::move(block));
    return function;
}

NativeRegisterFunctionIR make_two_block_fixture() {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values.push_back(NativeRegisterValueInfo{4U, NativeRegisterClass::GeneralPurpose});
    NativeRegisterBasicBlock entry;
    entry.id = 0U;
    auto declaration = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 0U);
    declaration.definitions.push_back(4U);
    entry.instructions.push_back(declaration);
    entry.successors.push_back(1U);

    NativeRegisterBasicBlock exit;
    exit.id = 1U;
    auto read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 0U);
    read.flow_block_id = 1U;
    read.flow_operation_id = (std::uint64_t{2U} << 32U) | 1U;
    read.uses.push_back(4U);
    exit.instructions.push_back(read);
    auto return_op = flow_instruction(NativeRegisterInstructionOriginKind::FlowReturn, 1U);
    return_op.flow_block_id = 1U;
    return_op.flow_operation_id = (std::uint64_t{2U} << 32U) | 2U;
    exit.instructions.push_back(return_op);

    function.blocks.push_back(std::move(entry));
    function.blocks.push_back(std::move(exit));
    return function;
}

NativeRegisterFunctionIR make_spill_fixture(std::size_t value_count = 7U) {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    NativeRegisterBasicBlock block;
    block.id = 0U;
    for (std::size_t index = 0U; index < value_count; ++index) {
        const auto value_id = static_cast<std::uint32_t>(index + 1U);
        function.values.push_back(NativeRegisterValueInfo{value_id, NativeRegisterClass::GeneralPurpose});
        auto definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare,
                                           static_cast<std::uint32_t>(index));
        definition.definitions.push_back(value_id);
        block.instructions.push_back(std::move(definition));
    }
    auto combined_read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead,
                                          static_cast<std::uint32_t>(value_count));
    for (std::size_t index = 0U; index < value_count; ++index)
        combined_read.uses.push_back(static_cast<std::uint32_t>(index + 1U));
    block.instructions.push_back(std::move(combined_read));
    auto return_op = flow_instruction(NativeRegisterInstructionOriginKind::FlowReturn,
                                      static_cast<std::uint32_t>(value_count + 1U));
    block.instructions.push_back(std::move(return_op));
    function.blocks.push_back(std::move(block));
    return function;
}

struct Fixture {
    NativeRegisterFunctionIR function;
    NativeRegisterLivenessResult liveness;
    NativeRegisterIntervalAnalysis intervals;
    NativeRegisterSegmentAnalysis segments;
    NativeRegisterAllocationPlan allocation;
};

Fixture build_fixture(NativeRegisterFunctionIR function) {
    Fixture fixture;
    fixture.function = std::move(function);
    fixture.liveness = compute_native_register_liveness(fixture.function);
    fixture.intervals = analyze_native_register_live_intervals(fixture.function, fixture.liveness);
    fixture.segments = split_native_register_live_ranges(
        fixture.function, fixture.liveness, fixture.intervals);
    NativeRegisterAllocationOptions options;
    options.prefer_caller_saved = true;
    options.spill_call_crossing = true;
    options.reserve_xmm0 = false;
    fixture.allocation = build_native_register_allocation_plan(
        fixture.function, fixture.liveness, fixture.intervals, fixture.segments, options);
    return fixture;
}

NativeRegisterFlowSegmentPlan build_plan(const Fixture& fixture) {
    return build_native_register_flow_segment_plan(fixture.function, fixture.liveness,
        fixture.intervals, fixture.segments, fixture.allocation);
}

const NativeRegisterFlowOperationLocation& find_operation(
    const NativeRegisterFlowSegmentPlan& plan, std::uint32_t operation_index) {
    const auto expected_id = (std::uint64_t{1U} << 32U) |
                             (static_cast<std::uint64_t>(operation_index) + 1U);
    const auto found = std::find_if(plan.operations.begin(), plan.operations.end(),
        [expected_id](const auto& operation) { return operation.flow_operation_id == expected_id; });
    if (found == plan.operations.end()) throw std::runtime_error("expected Flow operation was not mapped");
    return *found;
}

void test_synthetic_prefix_does_not_shift_flow_operation_identity() {
    const auto fixture = build_fixture(make_call_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 failed to build the valid call fixture plan");
    const auto& operation = find_operation(plan, 0U);
    require(operation.flow_operation_index == 0U && operation.native_instruction_index == 1U,
        "N112 conflated Flow operation index with native instruction index after a synthetic prefix");
}

void test_every_flow_operation_keeps_stable_id_block_and_both_positions() {
    const auto fixture = build_fixture(make_call_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded && plan.operations.size() == 4U,
        "N112 did not map every Flow-origin instruction in the fixture");
    for (const auto& operation : plan.operations) {
        require(operation.flow_operation_id != InvalidNativeFlowOperationId &&
                operation.flow_block_id == 0U &&
                operation.after_position == operation.before_position + 1U,
            "N112 operation record lost a stable identity or before/after position");
    }
}

void test_call_boundary_maps_pre_and_post_segments_separately() {
    const auto fixture = build_fixture(make_call_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 failed to reconcile call-boundary locations");
    const auto& call = find_operation(plan, 1U);
    require(call.before_locations.size() == 1U && call.after_locations.size() == 1U,
        "N112 omitted a call-crossing value from one side of the call");
    const auto before_segment = std::find_if(plan.segments.begin(), plan.segments.end(),
        [&call](const auto& item) {
            return item.segment.value_id == 7U && item.segment.block_id == 0U &&
                   item.segment.start <= call.before_position && item.segment.end >= call.before_position;
        });
    const auto after_segment = std::find_if(plan.segments.begin(), plan.segments.end(),
        [&call](const auto& item) {
            return item.segment.value_id == 7U && item.segment.block_id == 0U &&
                   item.segment.start <= call.after_position && item.segment.end >= call.after_position;
        });
    require(before_segment != plan.segments.end() && after_segment != plan.segments.end() &&
            before_segment->segment.segment_id != after_segment->segment.segment_id,
        "N112 did not preserve the call split as distinct before/after allocated segments");
}

void test_call_actions_bind_to_exact_flow_operation_and_placement() {
    const auto fixture = build_fixture(make_call_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 failed to bind call spill/reload actions");
    const auto before = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& binding) {
        return binding.action.kind == NativeSpillReloadActionKind::StoreBeforeCall;
    });
    const auto after = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& binding) {
        return binding.action.kind == NativeSpillReloadActionKind::ReloadAfterCall;
    });
    require(before != plan.actions.end() && after != plan.actions.end(),
        "N112 did not bind both call-preservation actions");
    require(before->flow_operation_id == ((std::uint64_t{1U} << 32U) | 2U) &&
            before->flow_operation_index == 1U &&
            before->placement == NativeRegisterFlowActionPlacement::BeforeOperation,
        "N112 bound StoreBeforeCall to an incorrect Flow operation or placement");
    require(after->flow_operation_id == before->flow_operation_id &&
            after->placement == NativeRegisterFlowActionPlacement::AfterOperation,
        "N112 bound ReloadAfterCall to an incorrect Flow operation or placement");
}

void test_pressure_fixture_produces_spills_and_both_spill_action_kinds() {
    const auto fixture = build_fixture(make_spill_fixture());
    require(fixture.allocation.succeeded && fixture.allocation.spill_slot_count > 0U,
        "N112 pressure fixture did not force any spill slots");
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 failed to reconcile spill actions under register pressure");
    const auto store = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& binding) {
        return binding.action.kind == NativeSpillReloadActionKind::StoreAfterDefinition;
    });
    const auto reload = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& binding) {
        return binding.action.kind == NativeSpillReloadActionKind::ReloadBeforeUse;
    });
    require(store != plan.actions.end() && reload != plan.actions.end(),
        "N112 did not bind definition-store and use-reload actions for spilled values");
    require(store->placement == NativeRegisterFlowActionPlacement::AfterOperation &&
            reload->placement == NativeRegisterFlowActionPlacement::BeforeOperation,
        "N112 assigned an incorrect placement to spill actions");
}

void test_high_pressure_call_preserves_spilled_pre_and_post_locations() {
    const auto fixture = build_fixture(make_pressure_call_fixture());
    require(fixture.allocation.succeeded && fixture.allocation.spill_slot_count > 0U,
        "high-pressure call fixture did not create spill slots");
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 rejected valid memory-resident call-boundary transfers under pressure");
    const auto store = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& binding) {
        return binding.action.kind == NativeSpillReloadActionKind::StoreBeforeCall &&
               binding.action.source.kind == NativeRegisterLocationKind::SpillSlot;
    });
    const auto reload = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& binding) {
        return binding.action.kind == NativeSpillReloadActionKind::ReloadAfterCall &&
               binding.action.destination.kind == NativeRegisterLocationKind::SpillSlot;
    });
    require(store != plan.actions.end() || reload != plan.actions.end(),
        "high-pressure call fixture did not exercise spill-slot call-boundary endpoints");
}

void test_segment_locations_remain_block_local_across_cfg_edges() {
    const auto fixture = build_fixture(make_two_block_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded && plan.operations.size() == 3U,
        "N112 failed to map Flow operations in a two-block CFG");
    const auto first_block = std::find_if(plan.operations.begin(), plan.operations.end(), [](const auto& op) {
        return op.flow_operation_id == ((std::uint64_t{1U} << 32U) | 1U);
    });
    const auto second_block = std::find_if(plan.operations.begin(), plan.operations.end(), [](const auto& op) {
        return op.flow_operation_id == ((std::uint64_t{2U} << 32U) | 1U);
    });
    require(first_block != plan.operations.end() && second_block != plan.operations.end() &&
            first_block->flow_block_id == 0U && second_block->flow_block_id == 1U,
        "N112 mixed stable Flow operation identities across basic blocks");
    require(!first_block->after_locations.empty() && !second_block->before_locations.empty() &&
            first_block->after_locations.front().value_id == 4U &&
            second_block->before_locations.front().value_id == 4U,
        "N112 failed to map the same virtual value in its distinct block-local segments");
}

void test_spill_actions_preserve_flow_operation_indices_after_native_mapping() {
    const auto fixture = build_fixture(make_spill_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 failed to map pressure actions");
    for (const auto& binding : plan.actions) {
        require(binding.flow_operation_id == ((std::uint64_t{1U} << 32U) |
                (static_cast<std::uint64_t>(binding.flow_operation_index) + 1U)),
            "N112 spill action does not match the stable operation ID derived from its source index");
    }
}

void test_allocated_segments_publish_physical_register_or_spill_locations() {
    const auto fixture = build_fixture(make_spill_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded && plan.segments.size() == fixture.segments.segments.size(),
        "N112 did not publish every allocated segment");
    for (const auto& mapped : plan.segments) {
        require(mapped.location.kind == NativeRegisterLocationKind::Register ||
                mapped.location.kind == NativeRegisterLocationKind::SpillSlot,
            "N112 published a non-machine location for an allocated segment");
        if (mapped.location.kind == NativeRegisterLocationKind::SpillSlot)
            require(mapped.location.spill_slot < fixture.allocation.spill_slot_count,
                "N112 published a spill slot outside the allocation plan");
    }
}

void test_flow_operations_are_sorted_by_stable_identity() {
    const auto fixture = build_fixture(make_spill_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 failed to produce operation records");
    require(std::is_sorted(plan.operations.begin(), plan.operations.end(), [](const auto& left, const auto& right) {
        return left.flow_operation_id < right.flow_operation_id;
    }), "N112 Flow operation records are not deterministically sorted");
}

void test_missing_allocation_segment_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    require(!fixture.allocation.segments.empty(), "test fixture has no allocated segments");
    fixture.allocation.segments.pop_back();
    require(!build_plan(fixture).succeeded, "N112 accepted a missing segment assignment");
}

void test_duplicate_allocated_segment_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    fixture.allocation.segments.push_back(fixture.allocation.segments.front());
    require(!build_plan(fixture).succeeded, "N112 accepted a duplicate allocated segment ID");
}

void test_physical_register_index_out_of_range_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    fixture.allocation.segments.front().assignment.register_index = 16U;
    require(!build_plan(fixture).succeeded, "N112 accepted a physical register index outside AMD64 range");
}

void test_spill_slot_out_of_range_is_rejected() {
    auto fixture = build_fixture(make_spill_fixture());
    auto spilled = std::find_if(fixture.allocation.segments.begin(), fixture.allocation.segments.end(),
        [](const auto& item) { return item.assignment.spilled; });
    require(spilled != fixture.allocation.segments.end(), "test fixture has no spilled segment");
    spilled->assignment.spill_slot = fixture.allocation.spill_slot_count;
    require(!build_plan(fixture).succeeded, "N112 accepted a spill slot outside the allocation plan");
}

void test_missing_segment_coverage_at_operation_position_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    auto& target = fixture.segments.segments.front();
    auto allocated = std::find_if(fixture.allocation.segments.begin(), fixture.allocation.segments.end(),
        [&target](const auto& item) { return item.segment.segment_id == target.segment_id; });
    require(allocated != fixture.allocation.segments.end(), "test fixture segment assignment is missing");
    const auto& first_flow = fixture.function.blocks[0].instructions[1];
    const auto position = fixture.intervals.instruction_positions[0][1];
    target.start = position + 1U;
    allocated->segment.start = target.start;
    (void)first_flow;
    require(!build_plan(fixture).succeeded, "N112 accepted an operation value without a covering segment");
}

void test_overlapping_segments_covering_one_position_are_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    auto extra = fixture.segments.segments.front();
    extra.segment_id += 1000U;
    fixture.segments.segments.push_back(extra);
    auto extra_assignment = fixture.allocation.segments.front();
    extra_assignment.segment = extra;
    extra_assignment.assignment.value_id = extra.segment_id;
    fixture.allocation.segments.push_back(extra_assignment);
    require(!build_plan(fixture).succeeded, "N112 accepted ambiguous overlapping segment coverage");
}

void test_duplicate_action_for_same_flow_operation_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    require(!fixture.allocation.actions.empty(), "test fixture has no call action to duplicate");
    fixture.allocation.actions.push_back(fixture.allocation.actions.front());
    require(!build_plan(fixture).succeeded, "N112 accepted duplicate action identity at one Flow operation");
}

void test_call_action_source_must_match_pre_operation_segment() {
    auto fixture = build_fixture(make_call_fixture());
    auto action = std::find_if(fixture.allocation.actions.begin(), fixture.allocation.actions.end(),
        [](const auto& item) { return item.kind == NativeSpillReloadActionKind::StoreBeforeCall; });
    require(action != fixture.allocation.actions.end(), "test fixture has no StoreBeforeCall action");
    action->source.register_index = static_cast<std::uint8_t>((action->source.register_index + 1U) % 16U);
    require(!build_plan(fixture).succeeded, "N112 accepted a call store source inconsistent with the pre-operation segment");
}

void test_call_action_destination_must_match_post_operation_segment() {
    auto fixture = build_fixture(make_call_fixture());
    auto action = std::find_if(fixture.allocation.actions.begin(), fixture.allocation.actions.end(),
        [](const auto& item) { return item.kind == NativeSpillReloadActionKind::ReloadAfterCall; });
    require(action != fixture.allocation.actions.end(), "test fixture has no ReloadAfterCall action");
    action->destination.register_index = static_cast<std::uint8_t>((action->destination.register_index + 1U) % 16U);
    require(!build_plan(fixture).succeeded, "N112 accepted a call reload destination inconsistent with the post-operation segment");
}

void test_action_on_synthetic_instruction_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    NativeSpillReloadAction action;
    action.kind = NativeSpillReloadActionKind::ReloadBeforeUse;
    action.value_id = 7U;
    action.block_id = 0U;
    action.instruction_index = 0U; // Synthetic parameter definition.
    action.slot_index = 0U;
    action.source.kind = NativeRegisterLocationKind::SpillSlot;
    action.destination.kind = NativeRegisterLocationKind::Temporary;
    fixture.allocation.actions.push_back(action);
    require(!build_plan(fixture).succeeded, "N112 bound an action to a synthetic instruction without a Flow anchor");
}

void test_incomplete_liveness_instruction_shape_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    fixture.liveness.instructions[0].pop_back();
    require(!build_plan(fixture).succeeded, "N112 accepted an incomplete liveness instruction table");
}

void test_incomplete_position_instruction_shape_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    fixture.intervals.instruction_positions[0].pop_back();
    require(!build_plan(fixture).succeeded, "N112 accepted an incomplete instruction-position table");
}

void test_failed_liveness_convergence_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    fixture.liveness.converged = false;
    require(!build_plan(fixture).succeeded, "N112 accepted a liveness result that did not converge");
}

void test_invalid_flow_origin_anchor_is_rejected() {
    auto fixture = build_fixture(make_call_fixture());
    fixture.function.blocks[0].instructions[1].flow_operation_id = InvalidNativeFlowOperationId;
    require(!build_plan(fixture).succeeded, "N112 accepted a Flow operation missing its stable anchor");
}

void test_xmm_segment_location_keeps_register_class() {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values.push_back(NativeRegisterValueInfo{9U, NativeRegisterClass::FloatingPoint});
    NativeRegisterBasicBlock block;
    block.id = 0U;
    auto declaration = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 0U);
    declaration.definitions.push_back(9U);
    block.instructions.push_back(declaration);
    auto read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 1U);
    read.uses.push_back(9U);
    block.instructions.push_back(read);
    function.blocks.push_back(block);
    const auto fixture = build_fixture(std::move(function));
    const auto plan = build_plan(fixture);
    require(plan.succeeded && !plan.segments.empty(), "N112 failed to map a floating-point segment");
    require(plan.segments.front().segment.register_class == NativeRegisterClass::FloatingPoint,
        "N112 lost the XMM register class while mapping a segment");
}

} // namespace

void run_n112_flow_segment_plan_tests(int& passed, int& failed) {
    run_case("N112A_synthetic_prefix_preserves_flow_index", test_synthetic_prefix_does_not_shift_flow_operation_identity, passed, failed);
    run_case("N112B_flow_operations_keep_stable_identity_and_positions", test_every_flow_operation_keeps_stable_id_block_and_both_positions, passed, failed);
    run_case("N112C_call_boundary_maps_pre_and_post_segments", test_call_boundary_maps_pre_and_post_segments_separately, passed, failed);
    run_case("N112D_call_actions_bind_exact_operation_and_placement", test_call_actions_bind_to_exact_flow_operation_and_placement, passed, failed);
    run_case("N112E_spills_generate_store_and_reload_action_bindings", test_pressure_fixture_produces_spills_and_both_spill_action_kinds, passed, failed);
    run_case("N112E2_call_pressure_accepts_spill_slot_transfer_endpoints", test_high_pressure_call_preserves_spilled_pre_and_post_locations, passed, failed);
    run_case("N112E3_cfg_segments_remain_block_local", test_segment_locations_remain_block_local_across_cfg_edges, passed, failed);
    run_case("N112F_spill_actions_preserve_stable_flow_indices", test_spill_actions_preserve_flow_operation_indices_after_native_mapping, passed, failed);
    run_case("N112G_allocated_segments_publish_machine_locations", test_allocated_segments_publish_physical_register_or_spill_locations, passed, failed);
    run_case("N112H_operation_records_are_deterministically_sorted", test_flow_operations_are_sorted_by_stable_identity, passed, failed);
    run_case("N112I_missing_segment_assignment_rejected", test_missing_allocation_segment_is_rejected, passed, failed);
    run_case("N112J_duplicate_allocated_segment_rejected", test_duplicate_allocated_segment_is_rejected, passed, failed);
    run_case("N112K_invalid_physical_register_rejected", test_physical_register_index_out_of_range_is_rejected, passed, failed);
    run_case("N112L_invalid_spill_slot_rejected", test_spill_slot_out_of_range_is_rejected, passed, failed);
    run_case("N112M_missing_segment_coverage_rejected", test_missing_segment_coverage_at_operation_position_is_rejected, passed, failed);
    run_case("N112N_overlapping_segment_coverage_rejected", test_overlapping_segments_covering_one_position_are_rejected, passed, failed);
    run_case("N112O_duplicate_action_identity_rejected", test_duplicate_action_for_same_flow_operation_is_rejected, passed, failed);
    run_case("N112P_call_store_source_location_mismatch_rejected", test_call_action_source_must_match_pre_operation_segment, passed, failed);
    run_case("N112Q_call_reload_destination_mismatch_rejected", test_call_action_destination_must_match_post_operation_segment, passed, failed);
    run_case("N112R_action_on_synthetic_instruction_rejected", test_action_on_synthetic_instruction_is_rejected, passed, failed);
    run_case("N112S_incomplete_liveness_shape_rejected", test_incomplete_liveness_instruction_shape_is_rejected, passed, failed);
    run_case("N112T_incomplete_position_shape_rejected", test_incomplete_position_instruction_shape_is_rejected, passed, failed);
    run_case("N112U_unconverged_liveness_rejected", test_failed_liveness_convergence_is_rejected, passed, failed);
    run_case("N112V_invalid_flow_anchor_rejected", test_invalid_flow_origin_anchor_is_rejected, passed, failed);
    run_case("N112W_xmm_register_class_preserved", test_xmm_segment_location_keeps_register_class, passed, failed);
}
