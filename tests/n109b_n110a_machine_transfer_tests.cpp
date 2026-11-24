// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/x64_location_transfer.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
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

NativeRegisterLocation location(NativeRegisterLocationKind kind, std::uint8_t reg = 0U,
                                std::uint32_t slot = 0U) {
    NativeRegisterLocation value;
    value.kind = kind;
    value.register_index = reg;
    value.spill_slot = slot;
    return value;
}

NativeX64FrameLocationMap frame_map() {
    NativeX64FrameLocationMap frame;
    frame.spill_slot_offsets.emplace(0U, -8);
    frame.spill_slot_offsets.emplace(1U, -24);
    frame.home_slot_offsets.emplace(0U, -16);
    frame.home_slot_offsets.emplace(1U, -32);
    return frame;
}

NativeX64ParallelCopyEncodingResult encode_one(
    NativeRegisterClass register_class,
    NativeRegisterLocation source,
    NativeRegisterLocation destination,
    NativeRegisterLocation temporary = location(NativeRegisterLocationKind::Temporary, 11U)) {
    NativeRegisterParallelCopyOperation operation;
    operation.value_id = 1U;
    operation.source = source;
    operation.destination = destination;
    return encode_native_x64_parallel_copy_schedule(
        {operation}, {{1U, register_class}}, temporary, frame_map());
}

NativeRegisterInstruction flow_instruction(
    NativeRegisterInstructionOriginKind origin,
    std::uint64_t id,
    std::uint32_t operation_index,
    std::vector<std::uint32_t> definitions = {},
    std::vector<std::uint32_t> uses = {},
    bool is_call = false) {
    NativeRegisterInstruction instruction;
    instruction.origin_kind = origin;
    (void)id;
    instruction.flow_operation_id = (static_cast<std::uint64_t>(1U) << 32U) |
        (static_cast<std::uint64_t>(operation_index) + 1U);
    instruction.flow_block_id = 0U;
    instruction.flow_operation_index = operation_index;
    instruction.source_span.start.byte_offset = operation_index * 2U;
    instruction.source_span.end.byte_offset = operation_index * 2U + 1U;
    instruction.definitions = std::move(definitions);
    instruction.uses = std::move(uses);
    instruction.is_call = is_call;
    return instruction;
}

NativeRegisterFunctionIR binding_fixture() {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values = {
        NativeRegisterValueInfo{1U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{2U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{3U, NativeRegisterClass::FloatingPoint}
    };
    NativeRegisterBasicBlock block;
    block.id = 0U;
    block.instructions.push_back(flow_instruction(
        NativeRegisterInstructionOriginKind::FlowWrite, 1U, 0U, {1U}));
    block.instructions.push_back(flow_instruction(
        NativeRegisterInstructionOriginKind::FlowCall, 2U, 1U, {}, {}, true));
    block.instructions.push_back(flow_instruction(
        NativeRegisterInstructionOriginKind::FlowDeclare, 3U, 2U, {2U}));
    block.instructions.push_back(flow_instruction(
        NativeRegisterInstructionOriginKind::FlowRead, 4U, 3U, {}, {3U}));
    function.blocks.push_back(std::move(block));
    return function;
}

NativeRegisterAllocationPlan all_action_kinds_plan() {
    NativeRegisterAllocationPlan plan;
    plan.succeeded = true;
    plan.actions = {
        NativeSpillReloadAction{NativeSpillReloadActionKind::StoreBeforeCall, 1U, 0U, 1U, 0U,
            location(NativeRegisterLocationKind::Register, 3U),
            location(NativeRegisterLocationKind::HomeSlot, 0U, 0U)},
        NativeSpillReloadAction{NativeSpillReloadActionKind::ReloadAfterCall, 1U, 0U, 1U, 0U,
            location(NativeRegisterLocationKind::HomeSlot, 0U, 0U),
            location(NativeRegisterLocationKind::Register, 6U)},
        NativeSpillReloadAction{NativeSpillReloadActionKind::StoreAfterDefinition, 2U, 0U, 2U, 1U,
            location(NativeRegisterLocationKind::Temporary, 0U),
            location(NativeRegisterLocationKind::SpillSlot, 0U, 1U)},
        NativeSpillReloadAction{NativeSpillReloadActionKind::ReloadBeforeUse, 3U, 0U, 3U, 1U,
            location(NativeRegisterLocationKind::SpillSlot, 0U, 1U),
            location(NativeRegisterLocationKind::Temporary, 0U)}
    };
    return plan;
}


void write_rel32(std::vector<std::uint8_t>& code, std::uint32_t displacement_offset,
                 std::uint32_t target_offset) {
    const auto delta = static_cast<std::int32_t>(target_offset - (displacement_offset + 4U));
    const auto raw = static_cast<std::uint32_t>(delta);
    for (unsigned byte = 0U; byte < 4U; ++byte)
        code.at(displacement_offset + byte) = static_cast<std::uint8_t>((raw >> (byte * 8U)) & 0xffU);
}

std::uint32_t read_rel32_target(const std::vector<std::uint8_t>& code,
                               std::uint32_t displacement_offset) {
    const std::uint32_t raw = static_cast<std::uint32_t>(code.at(displacement_offset)) |
        (static_cast<std::uint32_t>(code.at(displacement_offset + 1U)) << 8U) |
        (static_cast<std::uint32_t>(code.at(displacement_offset + 2U)) << 16U) |
        (static_cast<std::uint32_t>(code.at(displacement_offset + 3U)) << 24U);
    const auto delta = static_cast<std::int32_t>(raw);
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(displacement_offset + 4U) + delta);
}

X64FlowBranchAnchor test_branch_anchor(FlowEdgeId edge_id,
                                       X64FlowBranchEncoding encoding,
                                       bool conditional_taken,
                                       std::uint32_t instruction_offset,
                                       std::uint32_t displacement_offset,
                                       std::uint32_t target_offset,
                                       std::uint32_t code_size) {
    X64FlowBranchAnchor anchor;
    anchor.function_index = 0U;
    anchor.edge_id = edge_id;
    anchor.control_statement_id = 1U;
    anchor.control_construct_id = 1U;
    anchor.from_block_id = 0U;
    anchor.to_block_id = 1U;
    anchor.edge_kind = conditional_taken ? FlowEdgeKind::IfTrue : FlowEdgeKind::IfFalse;
    anchor.encoding = encoding;
    anchor.conditional_taken = conditional_taken;
    anchor.instruction_offset = instruction_offset;
    anchor.displacement_offset = displacement_offset;
    anchor.target_offset = target_offset;
    anchor.function_code_size = code_size;
    return anchor;
}

X64FlowEmitterAnchor test_operation_anchor(std::uint64_t operation_id,
                                            NativeRegisterInstructionOriginKind origin,
                                            std::uint32_t operation_index,
                                            std::uint32_t before_offset,
                                            std::uint32_t after_offset,
                                            std::uint32_t after_call_offset = InvalidNativeFlowOperationIndex,
                                            std::uint32_t code_size = 40U) {
    X64FlowEmitterAnchor anchor;
    anchor.function_index = 0U;
    anchor.flow_operation_id = operation_id;
    anchor.flow_block_id = 0U;
    anchor.flow_operation_index = operation_index;
    anchor.origin_kind = origin;
    anchor.source_span.start.byte_offset = operation_index * 2U;
    anchor.source_span.end.byte_offset = operation_index * 2U + 1U;
    anchor.machine_code_offset = after_offset;
    anchor.machine_code_before_offset = before_offset;
    anchor.machine_code_after_call_offset = after_call_offset;
    anchor.function_code_size = code_size;
    return anchor;
}

void test_gp_register_move() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::Register, 0U),
        location(NativeRegisterLocationKind::Register, 1U));
    require(result.succeeded && result.bytes == std::vector<std::uint8_t>({0x48U, 0x8BU, 0xC8U}),
        "N109B GP register move encoding is not MOV RCX,RAX");
}

void test_gp_extended_register_move() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::Register, 12U),
        location(NativeRegisterLocationKind::Register, 13U));
    require(result.succeeded && result.bytes == std::vector<std::uint8_t>({0x4DU, 0x8BU, 0xECU}),
        "N109B extended GP register move has incorrect REX/ModRM encoding");
}

void test_gp_home_load() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::HomeSlot, 0U, 0U),
        location(NativeRegisterLocationKind::Register, 0U));
    require(result.succeeded && result.bytes == std::vector<std::uint8_t>(
        {0x48U, 0x8BU, 0x85U, 0xF0U, 0xFFU, 0xFFU, 0xFFU}),
        "N109B GP home-slot load has incorrect RBP-relative encoding");
}

void test_gp_spill_store() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::Register, 10U),
        location(NativeRegisterLocationKind::SpillSlot, 0U, 1U));
    require(result.succeeded && result.bytes == std::vector<std::uint8_t>(
        {0x4CU, 0x89U, 0x95U, 0xE8U, 0xFFU, 0xFFU, 0xFFU}),
        "N109B GP spill store has incorrect RBP-relative encoding");
}

void test_xmm_register_move() {
    const auto result = encode_one(NativeRegisterClass::FloatingPoint,
        location(NativeRegisterLocationKind::Register, 0U),
        location(NativeRegisterLocationKind::Register, 1U));
    require(result.succeeded && result.bytes == std::vector<std::uint8_t>(
        {0xF3U, 0x0FU, 0x7EU, 0xC8U}),
        "N109B XMM register move is not the expected MOVQ encoding");
}

void test_xmm_extended_home_load() {
    const auto result = encode_one(NativeRegisterClass::FloatingPoint,
        location(NativeRegisterLocationKind::HomeSlot, 0U, 0U),
        location(NativeRegisterLocationKind::Register, 8U));
    require(result.succeeded && result.bytes == std::vector<std::uint8_t>(
        {0xF3U, 0x44U, 0x0FU, 0x7EU, 0x85U, 0xF0U, 0xFFU, 0xFFU, 0xFFU}),
        "N109B extended XMM home load has incorrect prefix or displacement");
}

void test_xmm_extended_spill_store() {
    const auto result = encode_one(NativeRegisterClass::FloatingPoint,
        location(NativeRegisterLocationKind::Register, 9U),
        location(NativeRegisterLocationKind::SpillSlot, 0U, 1U));
    require(result.succeeded && result.bytes == std::vector<std::uint8_t>(
        {0x66U, 0x44U, 0x0FU, 0xD6U, 0x8DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU}),
        "N109B extended XMM spill store has incorrect prefix or displacement");
}

void test_gp_memory_to_memory_uses_cycle_register() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::SpillSlot, 0U, 0U),
        location(NativeRegisterLocationKind::HomeSlot, 0U, 0U));
    const std::vector<std::uint8_t> expected{
        0x4CU, 0x8BU, 0x9DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0x4CU, 0x89U, 0x9DU, 0xF0U, 0xFFU, 0xFFU, 0xFFU};
    require(result.succeeded && result.bytes == expected &&
            result.memory_to_memory_expansions == 1U && result.machine_moves_emitted == 2U,
        "N109B GP memory-to-memory copy did not expand through its cycle register");
}

void test_xmm_memory_to_memory_uses_cycle_register() {
    const auto result = encode_one(NativeRegisterClass::FloatingPoint,
        location(NativeRegisterLocationKind::SpillSlot, 0U, 0U),
        location(NativeRegisterLocationKind::HomeSlot, 0U, 0U),
        location(NativeRegisterLocationKind::Temporary, 15U));
    const std::vector<std::uint8_t> expected{
        0xF3U, 0x44U, 0x0FU, 0x7EU, 0xBDU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x44U, 0x0FU, 0xD6U, 0xBDU, 0xF0U, 0xFFU, 0xFFU, 0xFFU};
    require(result.succeeded && result.bytes == expected &&
            result.memory_to_memory_expansions == 1U,
        "N109B XMM memory-to-memory copy did not expand through its cycle register");
}

void test_gp_resolver_cycle_is_emitted_with_reserved_temporary() {
    const auto scheduled = resolve_native_register_parallel_copies({
        NativeRegisterParallelCopy{1U,
            location(NativeRegisterLocationKind::Register, 0U),
            location(NativeRegisterLocationKind::Register, 1U)},
        NativeRegisterParallelCopy{2U,
            location(NativeRegisterLocationKind::Register, 1U),
            location(NativeRegisterLocationKind::Register, 0U)}
    }, location(NativeRegisterLocationKind::Temporary, 11U));
    require(scheduled.succeeded && scheduled.cycle_temporaries_used == 1U &&
            scheduled.operations.size() == 3U,
        "N109B GP cycle fixture was not resolved by the N80 scheduler");
    const auto encoded = encode_native_x64_parallel_copy_schedule(scheduled.operations,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    const std::vector<std::uint8_t> expected{
        0x4CU, 0x8BU, 0xD8U, // mov r11,rax
        0x48U, 0x8BU, 0xC1U, // mov rax,rcx
        0x49U, 0x8BU, 0xCBU  // mov rcx,r11
    };
    require(encoded.succeeded && encoded.bytes == expected && encoded.temporary_register_moves == 2U,
        "N109B did not encode the GP swap cycle through the resolver temporary");
}

void test_xmm_resolver_cycle_is_emitted_with_reserved_temporary() {
    const auto scheduled = resolve_native_register_parallel_copies({
        NativeRegisterParallelCopy{1U,
            location(NativeRegisterLocationKind::Register, 0U),
            location(NativeRegisterLocationKind::Register, 1U)},
        NativeRegisterParallelCopy{2U,
            location(NativeRegisterLocationKind::Register, 1U),
            location(NativeRegisterLocationKind::Register, 0U)}
    }, location(NativeRegisterLocationKind::Temporary, 11U));
    require(scheduled.succeeded && scheduled.cycle_temporaries_used == 1U &&
            scheduled.operations.size() == 3U,
        "N109B XMM cycle fixture was not resolved by the N80 scheduler");
    const auto encoded = encode_native_x64_parallel_copy_schedule(scheduled.operations,
        {{1U, NativeRegisterClass::FloatingPoint}, {2U, NativeRegisterClass::FloatingPoint}},
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    const std::vector<std::uint8_t> expected{
        0xF3U, 0x44U, 0x0FU, 0x7EU, 0xD8U, // movq xmm11,xmm0
        0xF3U, 0x0FU, 0x7EU, 0xC1U,       // movq xmm0,xmm1
        0xF3U, 0x41U, 0x0FU, 0x7EU, 0xCBU // movq xmm1,xmm11
    };
    require(encoded.succeeded && encoded.bytes == expected && encoded.temporary_register_moves == 2U,
        "N109B did not encode the XMM swap cycle through the resolver temporary");
}

void test_identity_copy_is_elided() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::Register, 3U),
        location(NativeRegisterLocationKind::Register, 3U));
    require(result.succeeded && result.bytes.empty() && result.machine_moves_emitted == 0U,
        "N109B identity copy emitted unnecessary machine bytes");
}

void test_missing_value_class_is_rejected() {
    NativeRegisterParallelCopyOperation operation{99U,
        location(NativeRegisterLocationKind::Register, 0U),
        location(NativeRegisterLocationKind::Register, 1U), false};
    const auto result = encode_native_x64_parallel_copy_schedule({operation}, {},
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N109BE0007",
        "N109B accepted a virtual value without a register class");
}

void test_reserved_gp_register_is_rejected() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::Register, 0U),
        location(NativeRegisterLocationKind::Register, 5U));
    require(!result.succeeded, "N109B accepted RBP as an allocatable GP transfer destination");
}

void test_missing_frame_slot_is_rejected() {
    NativeRegisterParallelCopyOperation operation{1U,
        location(NativeRegisterLocationKind::SpillSlot, 0U, 99U),
        location(NativeRegisterLocationKind::Register, 0U), false};
    const auto result = encode_native_x64_parallel_copy_schedule({operation},
        {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N109BE0004",
        "N109B accepted a stack slot missing from the frame map");
}

void test_positive_frame_displacement_is_rejected() {
    auto frame = frame_map();
    frame.home_slot_offsets[0U] = 8;
    NativeRegisterParallelCopyOperation operation{1U,
        location(NativeRegisterLocationKind::HomeSlot, 0U, 0U),
        location(NativeRegisterLocationKind::Register, 0U), false};
    const auto result = encode_native_x64_parallel_copy_schedule({operation},
        {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U), frame);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N109BE0005",
        "N109B accepted a positive RBP-relative frame displacement");
}

void test_mismatched_cycle_temporary_is_rejected() {
    const auto result = encode_one(NativeRegisterClass::GeneralPurpose,
        location(NativeRegisterLocationKind::Temporary, 10U),
        location(NativeRegisterLocationKind::Register, 0U),
        location(NativeRegisterLocationKind::Temporary, 11U));
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N109BE0002",
        "N109B accepted a temporary register not reserved by the caller");
}

void test_non_temporary_cycle_location_is_rejected() {
    NativeRegisterParallelCopyOperation operation{1U,
        location(NativeRegisterLocationKind::Register, 0U),
        location(NativeRegisterLocationKind::Register, 1U), false};
    const auto result = encode_native_x64_parallel_copy_schedule({operation},
        {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Register, 11U), frame_map());
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N109BE0006",
        "N109B accepted a non-temporary cycle location");
}

void test_bind_all_four_spill_reload_kinds() {
    const auto function = binding_fixture();
    const auto plan = all_action_kinds_plan();
    const auto result = bind_native_spill_reload_actions_to_flow_operations(function, plan);
    require(result.succeeded && result.bindings.size() == 4U,
        "N110A failed to bind all four allocator action kinds");
    std::size_t before = 0U;
    std::size_t after = 0U;
    for (const auto& binding : result.bindings) {
        if (binding.placement == NativeSpillReloadPlacement::BeforeOperation) ++before;
        else ++after;
    }
    require(before == 2U && after == 2U,
        "N110A assigned incorrect before/after placement to spill/reload actions");
}

void test_binding_preserves_flow_operation_identity_and_class() {
    const auto result = bind_native_spill_reload_actions_to_flow_operations(
        binding_fixture(), all_action_kinds_plan());
    require(result.succeeded, "N110A fixture binding failed");
    const auto call_before = std::find_if(result.bindings.begin(), result.bindings.end(),
        [](const NativeBoundSpillReloadAction& item) {
            return item.action.kind == NativeSpillReloadActionKind::StoreBeforeCall;
        });
    const auto float_reload = std::find_if(result.bindings.begin(), result.bindings.end(),
        [](const NativeBoundSpillReloadAction& item) {
            return item.action.kind == NativeSpillReloadActionKind::ReloadBeforeUse;
        });
    require(call_before != result.bindings.end() && call_before->flow_operation_id == ((static_cast<std::uint64_t>(1U) << 32U) | 2U) &&
            call_before->origin_kind == NativeRegisterInstructionOriginKind::FlowCall &&
            call_before->register_class == NativeRegisterClass::GeneralPurpose,
        "N110A lost call operation identity or GP class");
    require(float_reload != result.bindings.end() && float_reload->flow_operation_id == ((static_cast<std::uint64_t>(1U) << 32U) | 4U) &&
            float_reload->register_class == NativeRegisterClass::FloatingPoint,
        "N110A lost read operation identity or XMM class");
}

void test_unknown_value_is_rejected() {
    auto plan = all_action_kinds_plan();
    plan.actions.front().value_id = 99U;
    const auto result = bind_native_spill_reload_actions_to_flow_operations(binding_fixture(), plan);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110AE0004",
        "N110A accepted an action for an undeclared virtual value");
}

void test_unknown_block_is_rejected() {
    auto plan = all_action_kinds_plan();
    plan.actions.front().block_id = 9U;
    const auto result = bind_native_spill_reload_actions_to_flow_operations(binding_fixture(), plan);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110AE0002",
        "N110A accepted an action for an unknown CFG block");
}

void test_unknown_instruction_is_rejected() {
    auto plan = all_action_kinds_plan();
    plan.actions.front().instruction_index = 99U;
    const auto result = bind_native_spill_reload_actions_to_flow_operations(binding_fixture(), plan);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110AE0003",
        "N110A accepted an action for an unknown instruction index");
}

void test_action_cannot_bind_to_synthetic_instruction() {
    auto function = binding_fixture();
    auto& instruction = function.blocks[0].instructions[0];
    instruction.origin_kind = NativeRegisterInstructionOriginKind::Synthetic;
    instruction.flow_operation_id = InvalidNativeFlowOperationId;
    instruction.flow_block_id = InvalidNativeRegisterBlockId;
    instruction.flow_operation_index = InvalidNativeFlowOperationIndex;
    auto plan = all_action_kinds_plan();
    plan.actions.front().block_id = 0U;
    plan.actions.front().instruction_index = 0U;
    const auto result = bind_native_spill_reload_actions_to_flow_operations(function, plan);
    require(!result.succeeded && !result.diagnostics.empty(),
        "N110A bound a spill/reload action to a synthetic instruction");
}

void test_call_action_requires_call_boundary() {
    auto plan = all_action_kinds_plan();
    plan.actions.front().instruction_index = 0U; // FlowWrite, not FlowCall.
    const auto result = bind_native_spill_reload_actions_to_flow_operations(binding_fixture(), plan);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110AE0006",
        "N110A accepted a call-boundary save on a non-call instruction");
}

void test_store_action_requires_matching_definition() {
    auto plan = all_action_kinds_plan();
    plan.actions[2].value_id = 1U; // FlowDeclare defines value 2, not value 1.
    const auto result = bind_native_spill_reload_actions_to_flow_operations(binding_fixture(), plan);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110AE0006",
        "N110A accepted a store-after-definition without a matching definition");
}

void test_reload_action_requires_matching_use() {
    auto plan = all_action_kinds_plan();
    plan.actions[3].value_id = 2U; // FlowRead uses value 3, not value 2.
    const auto result = bind_native_spill_reload_actions_to_flow_operations(binding_fixture(), plan);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110AE0006",
        "N110A accepted a reload-before-use without a matching use");
}

void test_duplicate_action_is_rejected() {
    auto plan = all_action_kinds_plan();
    plan.actions.push_back(plan.actions.front());
    const auto result = bind_native_spill_reload_actions_to_flow_operations(binding_fixture(), plan);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110AE0007",
        "N110A accepted duplicate actions for one Flow operation");
}


void test_edge_bundle_inserts_island_for_conditional_taken_edge() {
    std::vector<std::uint8_t> code(24U, 0x90U);
    code[0] = 0x0FU; code[1] = 0x84U;
    write_rel32(code, 2U, 20U);
    const auto anchor = test_branch_anchor(1U, X64FlowBranchEncoding::ConditionalRel32,
        true, 0U, 2U, 20U, static_cast<std::uint32_t>(code.size()));
    NativeRegisterParallelCopyOperation copy;
    copy.value_id = 1U;
    copy.source = location(NativeRegisterLocationKind::Register, 0U);
    copy.destination = location(NativeRegisterLocationKind::Register, 1U);
    const auto result = insert_native_x64_edge_transfer_bundles(code, {{2U, 20U}}, {anchor}, {},
        {{1U, {copy}}}, {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U),
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    require(result.succeeded && result.edge_bundles_inserted == 1U &&
            result.transfer_islands_emitted == 1U && result.machine_moves_inserted == 1U,
        "N109B did not create a transfer island for a conditional-taken CFG edge");
    require(read_rel32_target(result.code, 2U) == 24U && result.code[24U] == 0x48U &&
            result.code[25U] == 0x8BU && result.code[26U] == 0xC8U,
        "N109B conditional-taken branch does not reach its encoded GP copy island");
    require(read_rel32_target(result.code, 28U) == 20U &&
            result.branch_anchors.size() == 1U &&
            result.branch_anchors.front().transfer_target_offset == 24U,
        "N109B transfer island did not return to the logical CFG target");
}

void test_edge_bundle_redirects_conditional_fallthrough_only() {
    std::vector<std::uint8_t> code(24U, 0x90U);
    code[0] = 0x0FU; code[1] = 0x84U;
    write_rel32(code, 2U, 20U);
    const auto anchor = test_branch_anchor(2U, X64FlowBranchEncoding::ConditionalRel32,
        false, 0U, 2U, 20U, static_cast<std::uint32_t>(code.size()));
    NativeRegisterParallelCopyOperation copy;
    copy.value_id = 1U;
    copy.source = location(NativeRegisterLocationKind::Register, 0U);
    copy.destination = location(NativeRegisterLocationKind::Register, 1U);
    const auto result = insert_native_x64_edge_transfer_bundles(code, {{2U, 20U}}, {anchor}, {},
        {{2U, {copy}}}, {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U),
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    require(result.succeeded && result.offset_adjustments.size() == 1U &&
            result.offset_adjustments.front().original_offset == 6U &&
            result.offset_adjustments.front().inserted_bytes == 5U,
        "N109B did not place the conditional-fallthrough redirect after Jcc");
    require(result.code[6U] == 0xE9U && read_rel32_target(result.code, 2U) == 25U &&
            read_rel32_target(result.code, 7U) == 29U,
        "N109B conditional-fallthrough redirect changed the taken path or misses its island: op6=" +
        std::to_string(result.code[6U]) + " taken=" + std::to_string(read_rel32_target(result.code, 2U)) +
        " redirect=" + std::to_string(read_rel32_target(result.code, 7U)));
    require(read_rel32_target(result.code, 33U) == 25U &&
            result.branch_anchors.front().transfer_instruction_offset == 6U &&
            result.branch_anchors.front().transfer_displacement_offset == 7U,
        "N109B fallthrough island does not return to the correct logical target");
}

void test_edge_bundle_fallthrough_does_not_capture_other_predecessors() {
    std::vector<std::uint8_t> code(24U, 0x90U);
    code[0] = 0xE9U;
    write_rel32(code, 1U, 10U);
    const auto anchor = test_branch_anchor(3U, X64FlowBranchEncoding::Fallthrough,
        false, 10U, InvalidNativeFlowOperationIndex, 20U,
        static_cast<std::uint32_t>(code.size()));
    NativeRegisterParallelCopyOperation copy;
    copy.value_id = 1U;
    copy.source = location(NativeRegisterLocationKind::Register, 0U);
    copy.destination = location(NativeRegisterLocationKind::Register, 1U);
    const auto result = insert_native_x64_edge_transfer_bundles(code, {{1U, 10U}}, {anchor}, {},
        {{3U, {copy}}}, {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U),
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    require(result.succeeded && result.code[10U] == 0xE9U &&
            read_rel32_target(result.code, 1U) == 15U,
        "N109B redirected an unrelated predecessor into the fallthrough edge bundle");
    require(read_rel32_target(result.code, 11U) == 29U &&
            read_rel32_target(result.code, 33U) == 25U,
        "N109B plain fallthrough island has an invalid redirect or return target: op10=" +
        std::to_string(result.code[10U]) + " predecessor=" + std::to_string(read_rel32_target(result.code, 1U)) +
        " redirect=" + std::to_string(read_rel32_target(result.code, 11U)) +
        " return=" + std::to_string(read_rel32_target(result.code, 33U)));
}

void test_edge_bundle_rebases_external_coff_relocation_after_inline_redirect() {
    std::vector<std::uint8_t> code(40U, 0x90U);
    code[0] = 0x0FU; code[1] = 0x84U;
    write_rel32(code, 2U, 30U);
    code[12] = 0xE8U; // An unrelated external CALL after the inserted fallthrough redirect.
    const auto anchor = test_branch_anchor(5U, X64FlowBranchEncoding::ConditionalRel32,
        false, 0U, 2U, 30U, static_cast<std::uint32_t>(code.size()));
    NativeRegisterParallelCopyOperation copy;
    copy.value_id = 1U;
    copy.source = location(NativeRegisterLocationKind::Register, 0U);
    copy.destination = location(NativeRegisterLocationKind::Register, 1U);
    NativeObjectRelocationInput external;
    external.offset = 13U;
    external.kind = ObjectRelocationKind::CoffRelative32;
    external.width = 4U;
    external.target_link_name = "__project_x_external_probe";
    external.target_signature_key = "test::external-probe";
    const auto result = insert_native_x64_edge_transfer_bundles(code, {{2U, 30U}}, {anchor}, {},
        {{5U, {copy}}}, {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U),
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map(), {external});
    require(result.succeeded && result.external_relocations.size() == 1U,
        "N109B did not preserve an external COFF relocation during edge insertion");
    const auto& rebased = result.external_relocations.front();
    require(rebased.offset == 18U && result.code[rebased.offset - 1U] == 0xE8U &&
            rebased.width == 4U && rebased.kind == ObjectRelocationKind::CoffRelative32 &&
            rebased.target_link_name == external.target_link_name &&
            rebased.target_signature_key == external.target_signature_key,
        "N109B failed to rebase external CALL relocation metadata after the inline redirect");
}

void test_edge_bundle_rejects_missing_exact_branch_anchor() {
    std::vector<std::uint8_t> code(16U, 0x90U);
    NativeRegisterParallelCopyOperation copy;
    copy.value_id = 1U;
    copy.source = location(NativeRegisterLocationKind::Register, 0U);
    copy.destination = location(NativeRegisterLocationKind::Register, 1U);
    const auto result = insert_native_x64_edge_transfer_bundles(code, {}, {}, {},
        {{99U, {copy}}}, {{1U, NativeRegisterClass::GeneralPurpose}},
        location(NativeRegisterLocationKind::Temporary, 11U),
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map());
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N109BI0007",
        "N109B accepted a non-empty transfer bundle without an exact FlowEdge anchor");
}

void test_spill_reload_insertion_places_all_actions_and_rebases_anchors() {
    auto binding = bind_native_spill_reload_actions_to_flow_operations(
        binding_fixture(), all_action_kinds_plan());
    require(binding.succeeded && binding.bindings.size() == 4U,
        "N110B spill/reload fixture did not bind all four action kinds");
    std::vector<std::uint8_t> code(40U, 0x90U);
    code[0] = 0xE9U;
    write_rel32(code, 1U, 28U);
    const auto anchors = std::vector<X64FlowEmitterAnchor>{
        test_operation_anchor((1ULL << 32U) | 1U, NativeRegisterInstructionOriginKind::FlowWrite, 0U, 3U, 4U),
        test_operation_anchor((1ULL << 32U) | 2U, NativeRegisterInstructionOriginKind::FlowCall, 1U, 8U, 16U, 12U),
        test_operation_anchor((1ULL << 32U) | 3U, NativeRegisterInstructionOriginKind::FlowDeclare, 2U, 20U, 24U),
        test_operation_anchor((1ULL << 32U) | 4U, NativeRegisterInstructionOriginKind::FlowRead, 3U, 28U, 36U)
    };
    const auto branch_anchor = test_branch_anchor(44U, X64FlowBranchEncoding::UnconditionalRel32,
        true, 0U, 1U, 28U, static_cast<std::uint32_t>(code.size()));
    const auto result = insert_native_x64_spill_reload_actions(code, {{1U, 28U}}, anchors,
        {branch_anchor}, binding.bindings, {{1U, NativeRegisterClass::GeneralPurpose},
                           {2U, NativeRegisterClass::GeneralPurpose},
                           {3U, NativeRegisterClass::FloatingPoint}},
        location(NativeRegisterLocationKind::Temporary, 0U),
        location(NativeRegisterLocationKind::Temporary, 0U), frame_map());
    require(result.succeeded && result.machine_actions_inserted == 4U &&
            result.machine_moves_inserted == 4U && result.offset_adjustments.size() == 4U,
        "N110B did not insert all four bound spill/reload machine actions");
    require(result.code.size() == 69U && read_rel32_target(result.code, 1U) == 49U,
        "N110B code insertion did not preserve the branch to the pre-use reload boundary");
    const auto call = std::find_if(result.operation_anchors.begin(), result.operation_anchors.end(),
        [](const X64FlowEmitterAnchor& anchor) { return anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowCall; });
    const auto read = std::find_if(result.operation_anchors.begin(), result.operation_anchors.end(),
        [](const X64FlowEmitterAnchor& anchor) { return anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowRead; });
    require(call != result.operation_anchors.end() && call->machine_code_before_offset == 8U &&
            call->machine_code_after_call_offset == 26U && call->machine_code_offset == 30U,
        "N110B did not rebase call-before, call-after and primary operation boundaries: before=" +
        std::to_string(call == result.operation_anchors.end() ? 9999U : call->machine_code_before_offset) +
        " afterCall=" + std::to_string(call == result.operation_anchors.end() ? 9999U : call->machine_code_after_call_offset) +
        " primary=" + std::to_string(call == result.operation_anchors.end() ? 9999U : call->machine_code_offset));
    require(read != result.operation_anchors.end() && read->machine_code_before_offset == 49U &&
            read->machine_code_offset == 65U,
        "N110B did not preserve the pre-read reload boundary after inserted transfers");
    require(result.branch_anchors.size() == 1U && result.branch_anchors.front().target_offset == 49U &&
            result.branch_anchors.front().function_code_size == result.code.size(),
        "N110B did not rebase CFG branch targets to execute reloads at the operation boundary");
}


void test_spill_then_edge_insertion_composes_and_rebases_cfg_targets() {
    auto binding = bind_native_spill_reload_actions_to_flow_operations(
        binding_fixture(), all_action_kinds_plan());
    std::vector<std::uint8_t> code(40U, 0x90U);
    code[0] = 0xE9U;
    write_rel32(code, 1U, 28U);
    code[12] = 0xE8U;
    NativeObjectRelocationInput external;
    external.offset = 13U;
    external.kind = ObjectRelocationKind::CoffRelative32;
    external.width = 4U;
    external.target_link_name = "__project_x_composed_external_call";
    external.target_signature_key = "test::composed-external-call";
    const auto operation_anchors = std::vector<X64FlowEmitterAnchor>{
        test_operation_anchor((1ULL << 32U) | 1U, NativeRegisterInstructionOriginKind::FlowWrite, 0U, 3U, 4U),
        test_operation_anchor((1ULL << 32U) | 2U, NativeRegisterInstructionOriginKind::FlowCall, 1U, 8U, 16U, 12U),
        test_operation_anchor((1ULL << 32U) | 3U, NativeRegisterInstructionOriginKind::FlowDeclare, 2U, 20U, 24U),
        test_operation_anchor((1ULL << 32U) | 4U, NativeRegisterInstructionOriginKind::FlowRead, 3U, 28U, 36U)
    };
    const auto original_branch = test_branch_anchor(44U, X64FlowBranchEncoding::UnconditionalRel32,
        true, 0U, 1U, 28U, static_cast<std::uint32_t>(code.size()));
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {1U, NativeRegisterClass::GeneralPurpose},
        {2U, NativeRegisterClass::GeneralPurpose},
        {3U, NativeRegisterClass::FloatingPoint}};
    const auto spill = insert_native_x64_spill_reload_actions(code, {{1U, 28U}}, operation_anchors,
        {original_branch}, binding.bindings, classes,
        location(NativeRegisterLocationKind::Temporary, 0U),
        location(NativeRegisterLocationKind::Temporary, 0U), frame_map(), {external});
    require(spill.succeeded && spill.code.size() == 69U && spill.branch_anchors.size() == 1U &&
            spill.branch_anchors.front().target_offset == 49U && spill.external_relocations.size() == 1U &&
            spill.external_relocations.front().offset > external.offset &&
            spill.code[spill.external_relocations.front().offset - 1U] == 0xE8U,
        "N110B did not rebase the incoming CFG edge and external CALL relocation to the transformed body");

    NativeRegisterParallelCopyOperation copy;
    copy.value_id = 1U;
    copy.source = location(NativeRegisterLocationKind::Register, 0U);
    copy.destination = location(NativeRegisterLocationKind::Register, 1U);
    const auto edge = insert_native_x64_edge_transfer_bundles(spill.code, spill.relative_branch_fixups,
        spill.branch_anchors, spill.operation_anchors, {{44U, {copy}}}, classes,
        location(NativeRegisterLocationKind::Temporary, 11U),
        location(NativeRegisterLocationKind::Temporary, 11U), frame_map(), spill.external_relocations);
    require(edge.succeeded && edge.edge_bundles_inserted == 1U && edge.code.size() == 77U,
        "N109B could not apply an edge bundle after N110B spill/reload insertion");
    require(read_rel32_target(edge.code, 1U) == 69U && edge.branch_anchors.front().transfer_target_offset == 69U &&
            read_rel32_target(edge.code, 73U) == 49U && edge.external_relocations.size() == 1U &&
            edge.external_relocations.front().offset == spill.external_relocations.front().offset &&
            edge.code[edge.external_relocations.front().offset - 1U] == 0xE8U,
        "N109B/N110B composed transform did not preserve branch targets and external CALL relocation");
    const auto read = std::find_if(edge.operation_anchors.begin(), edge.operation_anchors.end(),
        [](const X64FlowEmitterAnchor& anchor) { return anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowRead; });
    require(read != edge.operation_anchors.end() && read->machine_code_before_offset == 49U &&
            read->machine_code_offset == 65U && read->function_code_size == edge.code.size(),
        "N109B edge insertion invalidated the N110B pre-read reload anchor");
}

void test_spill_reload_insertion_rebases_external_call_relocation() {
    auto binding = bind_native_spill_reload_actions_to_flow_operations(
        binding_fixture(), all_action_kinds_plan());
    std::vector<std::uint8_t> code(48U, 0x90U);
    code[0] = 0xE9U;
    write_rel32(code, 1U, 36U);
    code[12] = 0xE8U;
    const auto anchors = std::vector<X64FlowEmitterAnchor>{
        test_operation_anchor((1ULL << 32U) | 1U, NativeRegisterInstructionOriginKind::FlowWrite, 0U, 3U, 4U,
            InvalidNativeFlowOperationIndex, 48U),
        test_operation_anchor((1ULL << 32U) | 2U, NativeRegisterInstructionOriginKind::FlowCall, 1U, 8U, 16U, 12U, 48U),
        test_operation_anchor((1ULL << 32U) | 3U, NativeRegisterInstructionOriginKind::FlowDeclare, 2U, 20U, 24U,
            InvalidNativeFlowOperationIndex, 48U),
        test_operation_anchor((1ULL << 32U) | 4U, NativeRegisterInstructionOriginKind::FlowRead, 3U, 28U, 36U,
            InvalidNativeFlowOperationIndex, 48U)
    };
    const auto branch_anchor = test_branch_anchor(46U, X64FlowBranchEncoding::UnconditionalRel32,
        true, 0U, 1U, 36U, static_cast<std::uint32_t>(code.size()));
    NativeObjectRelocationInput external;
    external.offset = 13U;
    external.kind = ObjectRelocationKind::CoffRelative32;
    external.width = 4U;
    external.target_link_name = "__project_x_external_call";
    external.target_signature_key = "test::external-call";
    const auto result = insert_native_x64_spill_reload_actions(code, {{1U, 36U}}, anchors,
        {branch_anchor}, binding.bindings, {{1U, NativeRegisterClass::GeneralPurpose},
            {2U, NativeRegisterClass::GeneralPurpose}, {3U, NativeRegisterClass::FloatingPoint}},
        location(NativeRegisterLocationKind::Temporary, 0U),
        location(NativeRegisterLocationKind::Temporary, 0U), frame_map(), {external});
    require(result.succeeded && result.external_relocations.size() == 1U,
        "N110B did not preserve an external COFF relocation during spill/reload insertion");
    const auto& rebased = result.external_relocations.front();
    require(rebased.offset > external.offset && rebased.offset + rebased.width <= result.code.size() &&
            result.code[rebased.offset - 1U] == 0xE8U &&
            rebased.kind == external.kind && rebased.width == external.width &&
            rebased.target_link_name == external.target_link_name &&
            rebased.target_signature_key == external.target_signature_key,
        "N110B failed to move an external CALL relocation with its CALL instruction");
}

void test_reload_before_use_requires_pre_operation_boundary() {
    auto binding = bind_native_spill_reload_actions_to_flow_operations(
        binding_fixture(), all_action_kinds_plan());
    const auto anchors = std::vector<X64FlowEmitterAnchor>{
        test_operation_anchor((1ULL << 32U) | 1U, NativeRegisterInstructionOriginKind::FlowWrite, 0U, 3U, 4U),
        test_operation_anchor((1ULL << 32U) | 2U, NativeRegisterInstructionOriginKind::FlowCall, 1U, 8U, 16U, 12U),
        test_operation_anchor((1ULL << 32U) | 3U, NativeRegisterInstructionOriginKind::FlowDeclare, 2U, 20U, 24U),
        test_operation_anchor((1ULL << 32U) | 4U, NativeRegisterInstructionOriginKind::FlowRead, 3U, InvalidNativeFlowOperationIndex, 36U)
    };
    const auto result = insert_native_x64_spill_reload_actions(std::vector<std::uint8_t>(40U, 0x90U),
        {}, anchors, {}, binding.bindings, {{1U, NativeRegisterClass::GeneralPurpose},
                                        {2U, NativeRegisterClass::GeneralPurpose},
                                        {3U, NativeRegisterClass::FloatingPoint}},
        location(NativeRegisterLocationKind::Temporary, 0U),
        location(NativeRegisterLocationKind::Temporary, 0U), frame_map());
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N110BI0008",
        "N110B accepted ReloadBeforeUse without a pre-operation machine boundary");
}

} // namespace

void run_n109b_n110a_machine_transfer_tests(int& passed, int& failed) {
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"N109BA_gp_register_move", test_gp_register_move},
        {"N109BB_gp_extended_register_move", test_gp_extended_register_move},
        {"N109BC_gp_home_load", test_gp_home_load},
        {"N109BD_gp_spill_store", test_gp_spill_store},
        {"N109BE_xmm_register_move", test_xmm_register_move},
        {"N109BF_xmm_extended_home_load", test_xmm_extended_home_load},
        {"N109BG_xmm_extended_spill_store", test_xmm_extended_spill_store},
        {"N109BH_gp_memory_to_memory", test_gp_memory_to_memory_uses_cycle_register},
        {"N109BI_xmm_memory_to_memory", test_xmm_memory_to_memory_uses_cycle_register},
        {"N109BJ_gp_resolver_cycle_emission", test_gp_resolver_cycle_is_emitted_with_reserved_temporary},
        {"N109BK_xmm_resolver_cycle_emission", test_xmm_resolver_cycle_is_emitted_with_reserved_temporary},
        {"N109BL_identity_copy_elided", test_identity_copy_is_elided},
        {"N109BM_missing_value_class_rejected", test_missing_value_class_is_rejected},
        {"N109BN_reserved_gp_register_rejected", test_reserved_gp_register_is_rejected},
        {"N109BO_missing_frame_slot_rejected", test_missing_frame_slot_is_rejected},
        {"N109BP_invalid_frame_displacement_rejected", test_positive_frame_displacement_is_rejected},
        {"N109BQ_temporary_mismatch_rejected", test_mismatched_cycle_temporary_is_rejected},
        {"N109BR_cycle_location_kind_rejected", test_non_temporary_cycle_location_is_rejected},
        {"N110AA_bind_all_action_kinds", test_bind_all_four_spill_reload_kinds},
        {"N110AB_preserve_operation_identity_and_class", test_binding_preserves_flow_operation_identity_and_class},
        {"N110AC_unknown_value_rejected", test_unknown_value_is_rejected},
        {"N110AD_unknown_block_rejected", test_unknown_block_is_rejected},
        {"N110AE_unknown_instruction_rejected", test_unknown_instruction_is_rejected},
        {"N110AF_synthetic_instruction_rejected", test_action_cannot_bind_to_synthetic_instruction},
        {"N110AG_call_requires_call_boundary", test_call_action_requires_call_boundary},
        {"N110AH_store_requires_definition", test_store_action_requires_matching_definition},
        {"N110AI_reload_requires_use", test_reload_action_requires_matching_use},
        {"N110AJ_duplicate_action_rejected", test_duplicate_action_is_rejected},
        {"N109BS_conditional_taken_edge_transfer_island", test_edge_bundle_inserts_island_for_conditional_taken_edge},
        {"N109BT_conditional_fallthrough_redirect", test_edge_bundle_redirects_conditional_fallthrough_only},
        {"N109BU_fallthrough_redirect_predecessor_isolation", test_edge_bundle_fallthrough_does_not_capture_other_predecessors},
        {"N109BV_missing_branch_anchor_rejected", test_edge_bundle_rejects_missing_exact_branch_anchor},
        {"N109BW_edge_insertion_rebases_external_coff_relocation", test_edge_bundle_rebases_external_coff_relocation_after_inline_redirect},
        {"N110BK_spill_reload_insertion_and_rebase", test_spill_reload_insertion_places_all_actions_and_rebases_anchors},
        {"N110BL_spill_then_edge_transform_composition", test_spill_then_edge_insertion_composes_and_rebases_cfg_targets},
        {"N110BM_reload_before_use_requires_pre_boundary", test_reload_before_use_requires_pre_operation_boundary},
        {"N110BN_spill_reload_rebases_external_call_relocation", test_spill_reload_insertion_rebases_external_call_relocation}
    };
    for (const auto& item : cases) run_case(item.first, item.second, passed, failed);
}
