// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_report.hpp"

#include "xfrontend/x64_location_transfer.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "xfrontend/win64_unwind_codegen.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
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

NativeRegisterInstruction flow_instruction(NativeRegisterInstructionOriginKind origin,
                                           std::uint32_t flow_index) {
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

struct Fixture {
    NativeRegisterFunctionIR function;
    NativeRegisterLivenessResult liveness;
    NativeRegisterIntervalAnalysis intervals;
    NativeRegisterSegmentAnalysis segments;
    NativeRegisterAllocationPlan allocation;
};

Fixture build_fixture(NativeRegisterFunctionIR function,
                      std::vector<std::uint8_t> reserved_gp = {},
                      std::vector<std::uint8_t> reserved_xmm = {}) {
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
    options.reserved_gp_registers = std::move(reserved_gp);
    options.reserved_xmm_registers = std::move(reserved_xmm);
    fixture.allocation = build_native_register_allocation_plan(
        fixture.function, fixture.liveness, fixture.intervals, fixture.segments, options);
    return fixture;
}

NativeRegisterFlowSegmentPlan build_plan(const Fixture& fixture) {
    return build_native_register_flow_segment_plan(fixture.function, fixture.liveness,
        fixture.intervals, fixture.segments, fixture.allocation);
}

NativeRegisterFunctionIR make_call_fixture() {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values.push_back(NativeRegisterValueInfo{7U, NativeRegisterClass::GeneralPurpose});
    NativeRegisterBasicBlock block;
    block.id = 0U;

    auto definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 0U);
    definition.definitions.push_back(7U);
    block.instructions.push_back(definition);

    auto call = flow_instruction(NativeRegisterInstructionOriginKind::FlowCall, 1U);
    call.is_call = true;
    block.instructions.push_back(call);

    auto read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 2U);
    read.uses.push_back(7U);
    block.instructions.push_back(read);

    auto return_op = flow_instruction(NativeRegisterInstructionOriginKind::FlowReturn, 3U);
    return_op.uses.push_back(7U);
    block.instructions.push_back(return_op);
    function.blocks.push_back(std::move(block));
    return function;
}

struct ManualReloadFixture {
    NativeRegisterFunctionIR function;
    NativeRegisterFlowSegmentPlan plan;
};

ManualReloadFixture make_manual_reload_fixture(NativeRegisterClass register_class,
                                               std::uint8_t busy_register) {
    ManualReloadFixture fixture;
    fixture.function.entry_block = 0U;
    fixture.function.values.push_back(NativeRegisterValueInfo{1U, register_class});
    fixture.function.values.push_back(NativeRegisterValueInfo{2U, register_class});
    NativeRegisterBasicBlock block;
    block.id = 0U;

    auto first_definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 0U);
    first_definition.definitions.push_back(1U);
    block.instructions.push_back(first_definition);
    auto second_definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 1U);
    second_definition.definitions.push_back(2U);
    block.instructions.push_back(second_definition);
    auto read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 2U);
    read.uses = {1U, 2U};
    block.instructions.push_back(read);
    fixture.function.blocks.push_back(std::move(block));

    NativeRegisterLocation spill;
    spill.kind = NativeRegisterLocationKind::SpillSlot;
    spill.spill_slot = 0U;
    NativeRegisterLocation live_register;
    live_register.kind = NativeRegisterLocationKind::Register;
    live_register.register_index = busy_register;

    const std::uint64_t read_id = (std::uint64_t{1U} << 32U) | 3U;
    NativeRegisterFlowOperationLocation operation;
    operation.flow_operation_id = read_id;
    operation.flow_block_id = 0U;
    operation.flow_operation_index = 2U;
    operation.native_instruction_index = 2U;
    operation.before_position = 2U;
    operation.after_position = 3U;
    operation.before_locations.push_back(
        NativeRegisterFlowValueLocation{1U, register_class, spill});
    operation.before_locations.push_back(
        NativeRegisterFlowValueLocation{2U, register_class, live_register});
    operation.after_locations = operation.before_locations;
    fixture.plan.operations.push_back(operation);

    NativeSpillReloadAction action;
    action.kind = NativeSpillReloadActionKind::ReloadBeforeUse;
    action.value_id = 1U;
    action.block_id = 0U;
    action.instruction_index = 2U;
    action.slot_index = 0U;
    action.source = spill;
    action.destination.kind = NativeRegisterLocationKind::Temporary;
    fixture.plan.actions.push_back(NativeRegisterFlowSegmentActionBinding{
        action, read_id, 0U, 2U, NativeRegisterInstructionOriginKind::FlowRead,
        register_class, NativeRegisterFlowActionPlacement::BeforeOperation});
    fixture.plan.succeeded = true;
    return fixture;
}

void write_rel32(std::vector<std::uint8_t>& code, std::size_t displacement_offset,
                 std::size_t target_offset) {
    const std::int64_t delta = static_cast<std::int64_t>(target_offset) -
        static_cast<std::int64_t>(displacement_offset + 4U);
    const auto encoded = static_cast<std::uint32_t>(static_cast<std::int32_t>(delta));
    for (unsigned byte = 0U; byte < 4U; ++byte)
        code[displacement_offset + byte] =
            static_cast<std::uint8_t>((encoded >> (byte * 8U)) & 0xffU);
}

std::size_t read_rel32_target(const std::vector<std::uint8_t>& code,
                              std::size_t displacement_offset) {
    std::uint32_t encoded = 0U;
    for (unsigned byte = 0U; byte < 4U; ++byte)
        encoded |= static_cast<std::uint32_t>(code[displacement_offset + byte]) << (byte * 8U);
    const auto displacement = static_cast<std::int32_t>(encoded);
    return static_cast<std::size_t>(static_cast<std::int64_t>(displacement_offset + 4U) + displacement);
}

void test_call_actions_convert_once_with_stable_identity_and_placement() {
    const auto fixture = build_fixture(make_call_fixture());
    const auto plan = build_plan(fixture);
    require(plan.succeeded, "N112 call fixture did not produce a successful segment plan");
    if (plan.actions.size() != 2U)
        throw std::runtime_error("N112 call fixture produced " + std::to_string(plan.actions.size()) +
            " actions instead of one store and one reload");

    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, plan, {}, {});
    require(resolved.succeeded && resolved.bindings.size() == plan.actions.size(),
        "N113 did not convert every N112 call action exactly once");
    const auto store = std::find_if(resolved.bindings.begin(), resolved.bindings.end(), [](const auto& item) {
        return item.action.kind == NativeSpillReloadActionKind::StoreBeforeCall;
    });
    const auto reload = std::find_if(resolved.bindings.begin(), resolved.bindings.end(), [](const auto& item) {
        return item.action.kind == NativeSpillReloadActionKind::ReloadAfterCall;
    });
    require(store != resolved.bindings.end() && reload != resolved.bindings.end() &&
            store->flow_operation_id == ((std::uint64_t{1U} << 32U) | 2U) &&
            reload->flow_operation_id == store->flow_operation_id &&
            store->placement == NativeSpillReloadPlacement::BeforeOperation &&
            reload->placement == NativeSpillReloadPlacement::AfterOperation &&
            !resolved.gp_cycle_temporary_required,
        "N113 changed call action identity, ordering placement, or scratch requirements");
}

void test_gp_temporary_avoids_live_registers_and_expression_reservations() {
    auto fixture = make_manual_reload_fixture(NativeRegisterClass::GeneralPurpose, 8U);
    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, fixture.plan, {9U}, {});
    if (!resolved.succeeded) {
        const std::string detail = resolved.diagnostics.empty() ? "no diagnostic" :
            resolved.diagnostics.front().code + " " + resolved.diagnostics.front().message;
        throw std::runtime_error("N113 GP resolver failed: " + detail);
    }
    if (resolved.gp_cycle_temporary.register_index != 10U)
        throw std::runtime_error("N113 chose GP temporary " +
            std::to_string(resolved.gp_cycle_temporary.register_index) + " instead of 10");
    require(resolved.succeeded && resolved.gp_cycle_temporary_required &&
            resolved.gp_cycle_temporary.kind == NativeRegisterLocationKind::Temporary &&
            resolved.gp_cycle_temporary.register_index == 10U,
        "N113 selected a GP temporary that was live or reserved for expression temporaries");
    require(resolved.bindings.size() == 1U &&
            resolved.bindings.front().action.destination.kind == NativeRegisterLocationKind::Temporary &&
            resolved.bindings.front().action.destination.register_index == 10U,
        "N113 did not rewrite the abstract GP temporary to the reserved physical scratch register");
}

void test_xmm_temporary_uses_matching_class_and_avoids_live_registers() {
    auto fixture = make_manual_reload_fixture(NativeRegisterClass::FloatingPoint, 0U);
    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, fixture.plan, {}, {1U});
    if (!resolved.succeeded) {
        const std::string detail = resolved.diagnostics.empty() ? "no diagnostic" :
            resolved.diagnostics.front().code + " " + resolved.diagnostics.front().message;
        throw std::runtime_error("N113 XMM resolver failed: " + detail);
    }
    if (resolved.xmm_cycle_temporary.register_index != 2U)
        throw std::runtime_error("N113 chose XMM temporary " +
            std::to_string(resolved.xmm_cycle_temporary.register_index) + " instead of 2");
    require(resolved.succeeded && resolved.xmm_cycle_temporary_required &&
            resolved.xmm_cycle_temporary.kind == NativeRegisterLocationKind::Temporary &&
            resolved.xmm_cycle_temporary.register_index == 2U &&
            !resolved.gp_cycle_temporary_required,
        "N113 did not choose an unoccupied class-correct XMM temporary");
    require(resolved.bindings.size() == 1U &&
            resolved.bindings.front().register_class == NativeRegisterClass::FloatingPoint &&
            resolved.bindings.front().action.destination.register_index == 2U,
        "N113 did not bind the XMM spill reload to its selected scratch register");
}

void test_duplicate_action_is_rejected_before_insertion() {
    auto fixture = make_manual_reload_fixture(NativeRegisterClass::GeneralPurpose, 8U);
    fixture.plan.actions.push_back(fixture.plan.actions.front());
    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, fixture.plan, {}, {});
    require(!resolved.succeeded && resolved.bindings.empty() && !resolved.diagnostics.empty(),
        "N113 accepted a duplicate action that could be inserted twice");
}

void test_mismatched_action_anchor_or_placement_is_rejected() {
    auto fixture = make_manual_reload_fixture(NativeRegisterClass::GeneralPurpose, 8U);
    fixture.plan.actions.front().placement = NativeRegisterFlowActionPlacement::AfterOperation;
    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, fixture.plan, {}, {});
    require(!resolved.succeeded && resolved.bindings.empty(),
        "N113 accepted a reload whose planned placement does not match its operation semantics");
}

void test_failed_n112_plan_is_rejected() {
    auto fixture = make_manual_reload_fixture(NativeRegisterClass::GeneralPurpose, 8U);
    fixture.plan.succeeded = false;
    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, fixture.plan, {}, {});
    require(!resolved.succeeded && resolved.bindings.empty(),
        "N113 accepted a failed N112 plan");
}

void test_missing_safe_scratch_register_is_rejected() {
    auto fixture = make_manual_reload_fixture(NativeRegisterClass::GeneralPurpose, 8U);
    const std::vector<std::uint8_t> all_gp_candidates{8U, 9U, 10U, 11U};
    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, fixture.plan, all_gp_candidates, {});
    require(!resolved.succeeded && resolved.bindings.empty() && !resolved.diagnostics.empty(),
        "N113 silently reused a reserved or live register when no GP scratch was available");
}

void test_scratch_selection_stays_inside_allocator_register_pools() {
    auto gp_fixture = make_manual_reload_fixture(NativeRegisterClass::GeneralPurpose, 15U);
    const auto gp = resolve_native_flow_segment_spill_reload_actions(
        gp_fixture.function, gp_fixture.plan, {}, {});
    require(gp.succeeded && gp.gp_cycle_temporary.register_index == 8U,
        "N113 selected a GP scratch outside the caller-saved allocator pool");

    auto xmm_fixture = make_manual_reload_fixture(NativeRegisterClass::FloatingPoint, 15U);
    const auto xmm = resolve_native_flow_segment_spill_reload_actions(
        xmm_fixture.function, xmm_fixture.plan, {}, {});
    require(xmm.succeeded && xmm.xmm_cycle_temporary.register_index == 1U,
        "N113 selected XMM0 or another register outside the expression-safe allocator pool");
}

void test_resolved_reload_insertion_rebases_branch_and_coff_relocation() {
    auto fixture = make_manual_reload_fixture(NativeRegisterClass::GeneralPurpose, 8U);
    const auto resolved = resolve_native_flow_segment_spill_reload_actions(
        fixture.function, fixture.plan, {}, {});
    require(resolved.succeeded && resolved.bindings.size() == 1U,
        "N113 could not resolve the reload action before code insertion");

    std::vector<std::uint8_t> code(32U, 0x90U);
    code[0] = 0xE9U;
    write_rel32(code, 1U, 8U);
    code[12] = 0xE8U;
    NativeObjectRelocationInput external;
    external.offset = 13U;
    external.kind = ObjectRelocationKind::CoffRelative32;
    external.width = 4U;
    external.target_link_name = "__n113_external_call_probe";

    X64FlowEmitterAnchor anchor;
    anchor.flow_operation_id = (std::uint64_t{1U} << 32U) | 3U;
    anchor.flow_block_id = 0U;
    anchor.flow_operation_index = 2U;
    anchor.origin_kind = NativeRegisterInstructionOriginKind::FlowRead;
    anchor.machine_code_offset = 16U;
    anchor.machine_code_before_offset = 8U;
    anchor.function_code_size = static_cast<std::uint32_t>(code.size());

    NativeX64FrameLocationMap frame;
    frame.spill_slot_offsets.emplace(0U, -8);
    const std::map<std::uint32_t, NativeRegisterClass> value_classes{
        {1U, NativeRegisterClass::GeneralPurpose},
        {2U, NativeRegisterClass::GeneralPurpose}};
    const auto inserted = insert_native_x64_spill_reload_actions(
        code, {{1U, 8U}}, {anchor}, {}, resolved.bindings, value_classes,
        resolved.gp_cycle_temporary, resolved.xmm_cycle_temporary, frame, {external});
    require(inserted.succeeded && inserted.machine_actions_inserted == 1U &&
            inserted.code.size() == code.size() + 7U,
        "N110B did not insert the resolved N113 reload at its stable Flow operation boundary");
    require(read_rel32_target(inserted.code, 1U) == 8U &&
            inserted.relative_branch_fixups.size() == 1U,
        "N113/N110B insertion did not retarget an incoming branch to execute the reload first");
    require(inserted.operation_anchors.size() == 1U &&
            inserted.operation_anchors.front().machine_code_before_offset == 8U &&
            inserted.operation_anchors.front().machine_code_offset == 23U,
        "N113/N110B insertion did not rebase before/after operation boundaries");
    require(inserted.external_relocations.size() == 1U &&
            inserted.external_relocations.front().offset == 20U &&
            inserted.external_relocations.front().kind == external.kind &&
            inserted.external_relocations.front().target_link_name == external.target_link_name &&
            inserted.code[inserted.external_relocations.front().offset - 1U] == 0xE8U,
        "N113/N110B insertion did not preserve and rebase the external COFF relocation");
}

X64CodegenResult compile_with_n113_anchor_validation(const std::string& source, bool enabled) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113 FunctionEmitter integration fixture did not parse");
    X64CodegenOptions options{"n113-emitter-anchor-proof", make_coff_x86_64_object_target()};
    options.validate_n113_flow_action_anchors = enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113 FunctionEmitter proof failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

X64CodegenResult compile_with_n113_frame_reservation(const std::string& source, bool enabled) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113 Flow frame reservation fixture did not parse");
    X64CodegenOptions options{"n113-flow-frame-reservation", make_coff_x86_64_object_target()};
    options.reserve_n113_flow_frame_slots = enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113 Flow frame reservation failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

std::string n113_emitter_pressure_source() {
    return "module N113Emitter; "
           "private function Helper(int p, int q) returns int { return p + q; } "
           "private function FloatHelper(double x, double y) returns double { return x + y; } "
           "private function Unused() returns int { return 77; } "
           "public function Main() returns int { "
           "int a=1; int b=2; int c=3; int d=4; int e=5; int f=6; "
           "double x=1.0; double y=2.0; double z=3.0; double w=4.0; "
           "double total = FloatHelper(x+y, z+w); "
           "int i=0; int result=0; "
           "while (i < 4) { if (i < 2) { result = result + Helper(a,b); } "
           "else { result = result + Helper(c,d); } i = i + 1; } "
           "return result + e + f; }";
}

void test_real_emitter_resolves_actions_to_call_read_write_and_cfg_anchors() {
    const auto result = compile_with_n113_anchor_validation(n113_emitter_pressure_source(), true);
    require(result.stats.native_n113_flow_segment_plans_validated == 4U,
        "N113 did not validate one flow-segment plan for each emitted function");
    require(result.stats.native_flow_spill_actions_planned > 0U &&
            result.stats.native_n113_flow_actions_resolved == result.stats.native_flow_spill_actions_planned,
        "N113 did not resolve every real-emitter spill/reload action exactly once");
    require(result.stats.native_n113_actions_anchor_validated == result.stats.native_n113_flow_actions_resolved,
        "N113 did not bind every resolved action to exactly one final operation/call anchor");
    require(result.stats.native_flow_emitter_anchors_emitted > 0U &&
            result.stats.native_cfg_branch_anchors_emitted > 0U,
        "N113 emitter fixture did not exercise Flow operation and CFG branch anchors");
}

void test_emitter_anchor_proof_is_opt_in_and_keeps_object_bytes_unchanged() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_anchor_validation(source, false);
    const auto checked = compile_with_n113_anchor_validation(source, true);
    require(baseline.stats.native_n113_flow_segment_plans_validated == 0U &&
            baseline.stats.native_n113_flow_actions_resolved == 0U &&
            baseline.stats.native_n113_actions_anchor_validated == 0U,
        "N113 proof mode must remain disabled by default");
    require(baseline.object_bytes == checked.object_bytes,
        "N113 anchor proof mode changed serialized COFF bytes");
}

NativeBoundSpillReloadAction make_location_audit_action(
    std::uint64_t operation_id, std::uint32_t value_id,
    NativeSpillReloadActionKind kind,
    NativeRegisterLocation source, NativeRegisterLocation destination) {
    NativeBoundSpillReloadAction binding;
    binding.flow_operation_id = operation_id;
    binding.action.value_id = value_id;
    binding.action.kind = kind;
    binding.register_class = NativeRegisterClass::GeneralPurpose;
    binding.action.source = source;
    binding.action.destination = destination;
    return binding;
}

NativeRegisterLocation make_location(NativeRegisterLocationKind kind,
                                     std::uint8_t register_index = 0U,
                                     std::uint32_t slot = 0U) {
    NativeRegisterLocation location;
    location.kind = kind;
    location.register_index = register_index;
    location.spill_slot = slot;
    return location;
}

void test_location_audit_distinguishes_register_matches_mismatches_and_unmapped_endpoints() {
    NativeX64MachineLocationPlan machine_locations;
    machine_locations.succeeded = true;
    NativeX64MachineValueLocation register_value;
    register_value.value_id = 1U;
    register_value.register_class = NativeRegisterClass::GeneralPurpose;
    register_value.active_location = make_location(NativeRegisterLocationKind::Register, 12U);
    machine_locations.values.emplace(register_value.value_id, register_value);
    NativeX64MachineValueLocation home_value;
    home_value.value_id = 2U;
    home_value.register_class = NativeRegisterClass::GeneralPurpose;
    home_value.active_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 3U);
    machine_locations.values.emplace(home_value.value_id, home_value);

    std::vector<NativeBoundSpillReloadAction> actions;
    actions.push_back(make_location_audit_action(1U, 1U,
        NativeSpillReloadActionKind::StoreBeforeCall,
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 0U)));
    actions.push_back(make_location_audit_action(2U, 1U,
        NativeSpillReloadActionKind::ReloadAfterCall,
        make_location(NativeRegisterLocationKind::Register, 10U),
        make_location(NativeRegisterLocationKind::Register, 12U)));
    actions.push_back(make_location_audit_action(3U, 2U,
        NativeSpillReloadActionKind::StoreBeforeCall,
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 3U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 3U)));
    actions.push_back(make_location_audit_action(4U, 99U,
        NativeSpillReloadActionKind::ReloadBeforeUse,
        make_location(NativeRegisterLocationKind::Temporary),
        make_location(NativeRegisterLocationKind::Temporary)));

    const auto audit = audit_native_x64_flow_action_locations(actions, machine_locations);
    require(audit.succeeded && audit.audited_actions == 4U,
        "N113-C did not audit each unique action");
    require(audit.register_endpoint_matches == 2U &&
            audit.register_endpoint_mismatches == 1U &&
            audit.unmapped_slot_or_temporary_endpoints == 3U &&
            audit.endpoints_without_persistent_binding == 2U &&
            audit.actions_without_persistent_binding == 1U,
        "N113-C misclassified register, memory, or non-binding action endpoints");
}

void test_location_audit_never_equates_same_numbered_n112_and_n111_home_slots() {
    NativeX64MachineLocationPlan machine_locations;
    machine_locations.succeeded = true;
    NativeX64MachineValueLocation value;
    value.value_id = 7U;
    value.register_class = NativeRegisterClass::GeneralPurpose;
    value.active_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U);
    machine_locations.values.emplace(value.value_id, value);
    const std::vector<NativeBoundSpillReloadAction> actions{make_location_audit_action(
        9U, 7U, NativeSpillReloadActionKind::StoreBeforeCall,
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U))};

    const auto audit = audit_native_x64_flow_action_locations(actions, machine_locations);
    require(audit.succeeded && audit.entries.size() == 1U,
        "N113-C rejected a structurally valid memory-location audit");
    require(audit.register_endpoint_matches == 0U &&
            audit.unmapped_slot_or_temporary_endpoints == 2U,
        "N113-C treated equal numeric slot indices as a proven physical-frame mapping");
}

void test_location_audit_rejects_duplicate_action_identity_and_invalid_plan() {
    NativeX64MachineLocationPlan machine_locations;
    machine_locations.succeeded = true;
    const auto action = make_location_audit_action(12U, 1U,
        NativeSpillReloadActionKind::ReloadBeforeUse,
        make_location(NativeRegisterLocationKind::Register, 8U),
        make_location(NativeRegisterLocationKind::Register, 9U));
    const std::vector<NativeBoundSpillReloadAction> duplicate{action, action};
    const auto duplicate_audit = audit_native_x64_flow_action_locations(duplicate, machine_locations);
    require(!duplicate_audit.succeeded && !duplicate_audit.diagnostics.empty() &&
            duplicate_audit.diagnostics.front().code == "N113L0003",
        "N113-C accepted duplicate Flow operation/value/action identities");
    machine_locations.succeeded = false;
    const auto invalid_plan = audit_native_x64_flow_action_locations({action}, machine_locations);
    require(!invalid_plan.succeeded && !invalid_plan.diagnostics.empty() &&
            invalid_plan.diagnostics.front().code == "N113L0001",
        "N113-C accepted an unsuccessful N111 machine-location plan");
}

X64CodegenResult compile_with_n113_location_audit(const std::string& source, bool enabled) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-C location-audit fixture did not parse");
    X64CodegenOptions options{"n113-machine-location-audit", make_coff_x86_64_object_target()};
    options.validate_n113_flow_action_locations = enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-C emitter audit failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

void test_real_emitter_location_audit_is_opt_in_accounted_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_location_audit(source, false);
    const auto audited = compile_with_n113_location_audit(source, true);
    require(baseline.stats.native_n113_actions_location_audited == 0U,
        "N113-C location audit must remain disabled by default");
    require(audited.stats.native_n113_flow_actions_resolved > 0U &&
            audited.stats.native_n113_actions_location_audited ==
                audited.stats.native_n113_flow_actions_resolved,
        "N113-C did not account for every resolved real-emitter action");
    const auto endpoints = audited.stats.native_n113_action_location_register_matches +
        audited.stats.native_n113_action_location_register_mismatches +
        audited.stats.native_n113_action_location_unmapped_endpoints +
        audited.stats.native_n113_action_location_unbound_endpoints;
    require(endpoints == audited.stats.native_n113_actions_location_audited * 2U,
        "N113-C location audit left one or more action endpoints unclassified");
    require(audited.stats.native_n113_action_location_unmapped_endpoints > 0U,
        "N113-C did not expose the unbound N112 stack/temporary endpoint namespace");
    require(baseline.object_bytes == audited.object_bytes,
        "N113-C location audit changed serialized COFF bytes");
}

NativeRegisterFlowValueLocation make_flow_value_location(
    std::uint32_t value_id, NativeRegisterClass register_class,
    NativeRegisterLocation location) {
    return NativeRegisterFlowValueLocation{value_id, register_class, location};
}

void test_operation_location_audit_classifies_before_after_positions() {
    NativeX64MachineLocationPlan machine_locations;
    machine_locations.succeeded = true;
    NativeX64MachineValueLocation register_value;
    register_value.value_id = 1U;
    register_value.register_class = NativeRegisterClass::GeneralPurpose;
    register_value.active_location = make_location(NativeRegisterLocationKind::Register, 12U);
    machine_locations.values.emplace(register_value.value_id, register_value);
    NativeX64MachineValueLocation home_value;
    home_value.value_id = 2U;
    home_value.register_class = NativeRegisterClass::GeneralPurpose;
    home_value.active_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U);
    machine_locations.values.emplace(home_value.value_id, home_value);

    NativeRegisterFlowOperationLocation operation;
    operation.flow_operation_id = 21U;
    operation.before_position = 10U;
    operation.after_position = 11U;
    operation.before_locations = {
        make_flow_value_location(1U, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::Register, 12U)),
        make_flow_value_location(2U, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U)),
        make_flow_value_location(99U, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::Temporary))};
    operation.after_locations = {
        make_flow_value_location(1U, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::Register, 10U)),
        make_flow_value_location(2U, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U)),
        make_flow_value_location(100U, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::Temporary))};
    NativeRegisterFlowSegmentPlan flow_plan;
    flow_plan.succeeded = true;
    flow_plan.operations.push_back(operation);

    const auto audit = audit_native_x64_flow_operation_locations(flow_plan, machine_locations);
    require(audit.succeeded && audit.audited_operations == 1U &&
            audit.audited_value_positions == 6U && audit.persistent_value_positions == 4U,
        "N113-D did not account for each before/after operation value position");
    require(audit.register_position_matches == 1U &&
            audit.register_position_mismatches == 1U &&
            audit.unmapped_slot_or_temporary_positions == 2U &&
            audit.unbound_value_positions == 2U &&
            audit.value_positions_without_persistent_binding == 2U,
        "N113-D misclassified N112 operation positions against N111 active locations");
}

void test_operation_location_audit_rejects_duplicate_and_inconsistent_plan_entries() {
    NativeX64MachineLocationPlan machine_locations;
    machine_locations.succeeded = true;
    NativeX64MachineValueLocation value;
    value.value_id = 1U;
    value.register_class = NativeRegisterClass::GeneralPurpose;
    value.active_location = make_location(NativeRegisterLocationKind::Register, 12U);
    machine_locations.values.emplace(value.value_id, value);

    NativeRegisterFlowOperationLocation operation;
    operation.flow_operation_id = 30U;
    operation.before_position = 1U;
    operation.after_position = 2U;
    operation.before_locations.push_back(make_flow_value_location(1U,
        NativeRegisterClass::GeneralPurpose, make_location(NativeRegisterLocationKind::Register, 12U)));
    NativeRegisterFlowSegmentPlan duplicate_operation_plan;
    duplicate_operation_plan.succeeded = true;
    duplicate_operation_plan.operations = {operation, operation};
    const auto duplicate_operation = audit_native_x64_flow_operation_locations(
        duplicate_operation_plan, machine_locations);
    require(!duplicate_operation.succeeded && !duplicate_operation.diagnostics.empty() &&
            duplicate_operation.diagnostics.front().code == "N113D0003",
        "N113-D accepted duplicate stable Flow operation identities");

    operation.before_locations.push_back(operation.before_locations.front());
    NativeRegisterFlowSegmentPlan duplicate_value_plan;
    duplicate_value_plan.succeeded = true;
    duplicate_value_plan.operations.push_back(operation);
    const auto duplicate_value = audit_native_x64_flow_operation_locations(
        duplicate_value_plan, machine_locations);
    require(!duplicate_value.succeeded && !duplicate_value.diagnostics.empty() &&
            duplicate_value.diagnostics.front().code == "N113D0004",
        "N113-D accepted duplicate values at one operation position");

    operation.before_locations.resize(1U);
    operation.before_locations.front().register_class = NativeRegisterClass::FloatingPoint;
    NativeRegisterFlowSegmentPlan class_mismatch_plan;
    class_mismatch_plan.succeeded = true;
    class_mismatch_plan.operations.push_back(operation);
    const auto class_mismatch = audit_native_x64_flow_operation_locations(
        class_mismatch_plan, machine_locations);
    require(!class_mismatch.succeeded && !class_mismatch.diagnostics.empty() &&
            class_mismatch.diagnostics.front().code == "N113D0004",
        "N113-D accepted an N111/N112 register-class mismatch");
}

X64CodegenResult compile_with_n113_operation_location_audit(
    const std::string& source, bool enabled) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-D operation-location fixture did not parse");
    X64CodegenOptions options{"n113-operation-location-audit", make_coff_x86_64_object_target()};
    options.validate_n113_flow_operation_locations = enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-D operation-location audit failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

void test_real_emitter_operation_location_audit_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_operation_location_audit(source, false);
    const auto audited = compile_with_n113_operation_location_audit(source, true);
    require(baseline.stats.native_n113_flow_operations_location_audited == 0U &&
            baseline.stats.native_n113_flow_value_positions_location_audited == 0U,
        "N113-D operation-location audit must remain disabled by default");
    require(audited.stats.native_n113_flow_segment_plans_validated == 4U &&
            audited.stats.native_n113_flow_operations_location_audited > 0U &&
            audited.stats.native_n113_flow_value_positions_location_audited > 0U,
        "N113-D did not audit real emitter operation positions");
    const auto classified_positions =
        audited.stats.native_n113_flow_positions_register_matches +
        audited.stats.native_n113_flow_positions_register_mismatches +
        audited.stats.native_n113_flow_positions_unmapped +
        audited.stats.native_n113_flow_positions_unbound;
    require(classified_positions == audited.stats.native_n113_flow_value_positions_location_audited,
        "N113-D left operation positions unclassified");
    require(baseline.object_bytes == audited.object_bytes,
        "N113-D operation-location audit changed serialized COFF bytes");
}

X64CodegenResult compile_with_n113_emitter_operand_crosswalk(
    const std::string& source, bool enabled) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-E emitter-operand fixture did not parse");
    X64CodegenOptions options{"n113-emitter-operand-crosswalk", make_coff_x86_64_object_target()};
    options.validate_n113_flow_emitter_operand_locations = enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-E emitter-operand crosswalk failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

void test_real_emitter_operand_crosswalk_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_emitter_operand_crosswalk(source, false);
    const auto audited = compile_with_n113_emitter_operand_crosswalk(source, true);
    require(baseline.stats.native_n113_flow_emitter_operations_mapped == 0U &&
            baseline.stats.native_n113_flow_emitter_reads_mapped == 0U &&
            baseline.stats.native_n113_flow_emitter_writes_mapped == 0U &&
            baseline.stats.native_n113_flow_emitter_calls_mapped == 0U,
        "N113-E emitter-operand crosswalk must remain disabled by default");
    require(audited.stats.native_n113_flow_emitter_operations_mapped > 0U &&
            audited.stats.native_n113_flow_emitter_reads_mapped > 0U &&
            audited.stats.native_n113_flow_emitter_writes_mapped > 0U &&
            audited.stats.native_n113_flow_emitter_calls_mapped > 0U,
        "N113-E did not cross-check real Flow reads, writes and calls");
    require(audited.stats.native_n113_flow_emitter_operand_matches +
            audited.stats.native_n113_flow_emitter_operand_register_mismatches +
            audited.stats.native_n113_flow_emitter_operand_unmapped > 0U,
        "N113-E did not compare any actual emitter operand to its N112 location");
    for (const auto& anchor : audited.flow_anchors) {
        if (anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowCall) {
            require(anchor.machine_code_call_instruction_offset != InvalidNativeFlowOperationIndex &&
                    anchor.machine_code_call_instruction_offset + 5U == anchor.machine_code_after_call_offset,
                "N113-E FlowCall anchor does not retain its exact CALL instruction range");
        }
        if (anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None) {
            require(anchor.emitted_operand_value_id != 0U &&
                    anchor.machine_code_before_offset < anchor.machine_code_offset,
                "N113-E value operand has no stable value ID or machine-code interval");
        }
    }
    require(baseline.object_bytes == audited.object_bytes,
        "N113-E emitter-operand crosswalk changed serialized COFF bytes");
}

void test_flow_frame_slots_are_bound_after_emitter_namespaces() {
    NativeRegisterAllocationPlan emitter;
    emitter.succeeded = true;
    emitter.spill_slot_count = 2U;
    emitter.home_slot_count = 3U;
    NativeRegisterAllocationPlan flow;
    flow.succeeded = true;
    flow.spill_slot_count = 4U;
    flow.home_slot_count = 2U;
    const auto bound = plan_native_x64_flow_frame_binding(emitter, flow, 64U);
    require(bound.succeeded && bound.flow_spill_slots_reserved == 4U &&
            bound.flow_home_slots_reserved == 2U,
        "N113-F did not reserve the requested N112 Flow spill and home slots");
    require(bound.frame_plan.spill_slot_offsets.size() == 6U &&
            bound.frame_plan.home_slot_offsets.size() == 5U &&
            bound.flow_frame_locations.spill_slot_offsets.size() == 4U &&
            bound.flow_frame_locations.home_slot_offsets.size() == 2U,
        "N113-F combined frame omitted a slot or produced an incomplete Flow map");
    std::set<std::int32_t> emitter_offsets;
    std::set<std::int32_t> flow_offsets;
    for (std::uint32_t slot = 0U; slot < emitter.spill_slot_count; ++slot)
        emitter_offsets.insert(bound.frame_plan.spill_slot_offsets[slot]);
    for (const auto& item : bound.flow_frame_locations.spill_slot_offsets) {
        require(item.first < flow.spill_slot_count && item.second < 0 && item.second % 8 == 0,
            "N113-F Flow spill slot is outside its namespace or not a valid RBP displacement");
        flow_offsets.insert(item.second);
    }
    for (std::uint32_t slot = 0U; slot < emitter.home_slot_count; ++slot)
        emitter_offsets.insert(bound.frame_plan.home_slot_offsets[slot]);
    for (const auto& item : bound.flow_frame_locations.home_slot_offsets) {
        require(item.first < flow.home_slot_count && item.second < 0 && item.second % 8 == 0,
            "N113-F Flow home slot is outside its namespace or not a valid RBP displacement");
        flow_offsets.insert(item.second);
    }
    for (const auto offset : flow_offsets)
        require(emitter_offsets.find(offset) == emitter_offsets.end(),
            "N113-F Flow slot aliases an existing emitter spill/home frame slot");
    require(bound.frame_plan.frame_size_bytes % 16U == 0U,
        "N113-F frame binding broke Win64 stack alignment");

    NativeRegisterAllocationPlan overflow = emitter;
    overflow.spill_slot_count = std::numeric_limits<std::uint32_t>::max();
    const auto rejected = plan_native_x64_flow_frame_binding(overflow, flow, 0U);
    require(!rejected.succeeded && !rejected.diagnostics.empty(),
        "N113-F did not reject overflowing combined slot counts");
}

void test_flow_frame_reservation_is_opt_in_and_drives_probe_unwind() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_frame_reservation(source, false);
    const auto reserved = compile_with_n113_frame_reservation(source, true);
    require(baseline.stats.native_n113_flow_frame_spill_slots_reserved == 0U &&
            baseline.stats.native_n113_flow_frame_home_slots_reserved == 0U &&
            reserved.stats.native_n113_flow_frame_spill_slots_reserved +
                reserved.stats.native_n113_flow_frame_home_slots_reserved > 0U,
        "N113-F reservation must remain disabled by default and reserve actual N112 slots when enabled");
    require(baseline.stats.win64_unwind_records_emitted > 0U &&
            reserved.stats.win64_unwind_records_emitted == baseline.stats.win64_unwind_records_emitted,
        "N113-F changed Win64 unwind record emission while extending the frame");
    require(reserved.stats.stack_frame_bytes > baseline.stats.stack_frame_bytes &&
            reserved.stats.native_n113_flow_actions_resolved == 0U &&
            baseline.object_bytes != reserved.object_bytes,
        "N113-F must enlarge the real emitter frame without resolving or inserting N112 transfers");

    NativeRegisterAllocationPlan emitter;
    emitter.succeeded = true;
    NativeRegisterAllocationPlan large_flow;
    large_flow.succeeded = true;
    large_flow.spill_slot_count = 600U;
    const auto large_frame = plan_native_x64_flow_frame_binding(emitter, large_flow, 0U);
    require(large_frame.succeeded && large_frame.frame_plan.frame_size_bytes >= 4096U,
        "N113-F large reserved frame did not reach the Win64 stack-probe threshold");
    const auto probe = build_win64_stack_probe_plan(large_frame.frame_plan.frame_size_bytes);
    require(probe.succeeded && !probe.probe_offsets.empty(),
        "N113-F large Flow frame did not produce the existing page-by-page probe plan");
    const auto unwind = build_win64_frame_unwind_plan(
        0U, 32U, large_frame.frame_plan.frame_size_bytes, true);
    require(unwind.succeeded(),
        "N113-F reserved frame size was rejected by the Win64 unwind planner");
}

struct G1Fixture {
    NativeRegisterFunctionIR function;
    NativeRegisterFlowSegmentPlan flow_plan;
    NativeX64MachineLocationPlan machine_locations;
    std::vector<X64FlowEmitterAnchor> anchors;
    std::vector<NativeBoundSpillReloadAction> bindings;
    NativeX64FrameLocationMap emitter_frame;
    std::map<std::uint32_t, std::int32_t> local_home_offsets;
    NativeX64FrameLocationMap flow_frame;
    std::uint32_t emitter_spill_slots = 0U;
    std::uint32_t emitter_home_slots = 0U;
    std::uint32_t local_home_slots = 0U;
};

NativeRegisterFlowValueLocation g1_flow_position(
    std::uint32_t value_id, NativeRegisterClass register_class,
    NativeRegisterLocationKind kind, std::uint8_t reg = 0U, std::uint32_t slot = 0U) {
    return NativeRegisterFlowValueLocation{value_id, register_class, make_location(kind, reg, slot)};
}

NativeBoundSpillReloadAction g1_bound_action(
    std::uint64_t operation_id, std::uint32_t value_id,
    NativeSpillReloadActionKind kind, std::uint32_t instruction_index,
    NativeRegisterClass register_class, NativeRegisterLocation source,
    NativeRegisterLocation destination, NativeSpillReloadPlacement placement,
    std::uint32_t slot_index) {
    NativeBoundSpillReloadAction binding;
    binding.flow_operation_id = operation_id;
    binding.origin_kind = kind == NativeSpillReloadActionKind::StoreBeforeCall ||
        kind == NativeSpillReloadActionKind::ReloadAfterCall
        ? NativeRegisterInstructionOriginKind::FlowCall
        : kind == NativeSpillReloadActionKind::StoreAfterDefinition
            ? NativeRegisterInstructionOriginKind::FlowWrite
            : NativeRegisterInstructionOriginKind::FlowRead;
    binding.register_class = register_class;
    binding.placement = placement;
    binding.action.kind = kind;
    binding.action.value_id = value_id;
    binding.action.block_id = 0U;
    binding.action.instruction_index = instruction_index;
    binding.action.slot_index = slot_index;
    binding.action.source = source;
    binding.action.destination = destination;
    return binding;
}

G1Fixture make_g1_call_rebinding_fixture(NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose,
                                         std::uint8_t live_before = 3U,
                                         std::uint8_t live_after = 2U,
                                         NativeRegisterLocationKind emitter_location_kind = NativeRegisterLocationKind::HomeSlot) {
    G1Fixture fixture;
    fixture.function.entry_block = 0U;
    fixture.function.values.push_back(NativeRegisterValueInfo{7U, register_class});
    NativeRegisterBasicBlock block;
    block.id = 4U;
    auto call = flow_instruction(NativeRegisterInstructionOriginKind::FlowCall, 9U);
    call.flow_block_id = 4U;
    call.is_call = true;
    block.instructions.push_back(call);
    fixture.function.blocks.push_back(block);

    NativeRegisterFlowOperationLocation operation;
    operation.flow_operation_id = call.flow_operation_id;
    operation.flow_block_id = call.flow_block_id;
    operation.flow_operation_index = call.flow_operation_index;
    operation.native_instruction_index = 0U;
    operation.before_position = 10U;
    operation.after_position = 11U;
    operation.before_locations.push_back(g1_flow_position(7U, register_class,
        NativeRegisterLocationKind::Register, live_before));
    operation.after_locations.push_back(g1_flow_position(7U, register_class,
        NativeRegisterLocationKind::Register, live_after));
    fixture.flow_plan.operations.push_back(operation);

    NativeRegisterLocation flow_home = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 0U);
    NativeRegisterLocation n112_before = make_location(NativeRegisterLocationKind::Register, live_before);
    NativeRegisterLocation n112_after = make_location(NativeRegisterLocationKind::Register, live_after);
    fixture.flow_plan.actions.push_back(NativeRegisterFlowSegmentActionBinding{
        NativeSpillReloadAction{NativeSpillReloadActionKind::StoreBeforeCall, 7U, 0U, 0U, 0U,
            n112_before, flow_home},
        call.flow_operation_id, 4U, 9U, NativeRegisterInstructionOriginKind::FlowCall,
        register_class, NativeRegisterFlowActionPlacement::BeforeOperation});
    fixture.flow_plan.actions.push_back(NativeRegisterFlowSegmentActionBinding{
        NativeSpillReloadAction{NativeSpillReloadActionKind::ReloadAfterCall, 7U, 0U, 0U, 0U,
            flow_home, n112_after},
        call.flow_operation_id, 4U, 9U, NativeRegisterInstructionOriginKind::FlowCall,
        register_class, NativeRegisterFlowActionPlacement::AfterOperation});

    X64FlowEmitterAnchor anchor;
    anchor.function_index = 0U;
    anchor.flow_operation_id = call.flow_operation_id;
    anchor.flow_block_id = 4U;
    anchor.flow_operation_index = 9U;
    anchor.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
    anchor.machine_code_before_offset = 8U;
    anchor.machine_code_offset = 20U;
    anchor.machine_code_call_instruction_offset = 12U;
    anchor.machine_code_after_call_offset = 17U;
    anchor.function_code_size = 32U;
    fixture.anchors.push_back(anchor);

    NativeX64MachineValueLocation machine_value;
    machine_value.value_id = 7U;
    machine_value.register_class = register_class;
    machine_value.active_location = make_location(emitter_location_kind, 0U, 1U);
    machine_value.canonical_home = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 1U);
    fixture.machine_locations.succeeded = true;
    fixture.machine_locations.values.emplace(7U, machine_value);

    fixture.local_home_offsets = {{0U, -8}, {1U, -16}};
    fixture.local_home_slots = 2U;
    fixture.flow_frame.home_slot_offsets.emplace(0U, -24);
    fixture.flow_plan.succeeded = true;
    fixture.bindings.push_back(g1_bound_action(call.flow_operation_id, 7U,
        NativeSpillReloadActionKind::StoreBeforeCall, 0U, register_class,
        n112_before, flow_home, NativeSpillReloadPlacement::BeforeOperation, 0U));
    fixture.bindings.push_back(g1_bound_action(call.flow_operation_id, 7U,
        NativeSpillReloadActionKind::ReloadAfterCall, 0U, register_class,
        flow_home, n112_after, NativeSpillReloadPlacement::AfterOperation, 0U));
    return fixture;
}

void test_g1_call_actions_rebind_to_n111_home_and_rebase_flow_home() {
    const auto fixture = make_g1_call_rebinding_fixture();
    const auto rebound = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, 0U, 0U, fixture.local_home_slots, {}, {});
    require(rebound.succeeded && rebound.rebound_actions == 2U &&
            rebound.memory_to_memory_actions == 2U && rebound.gp_cycle_temporary_required &&
            rebound.gp_cycle_temporary.kind == NativeRegisterLocationKind::Temporary &&
            rebound.gp_cycle_temporary.register_index >= 8U && rebound.gp_cycle_temporary.register_index <= 11U,
        "G1 did not rebind both call actions or select a safe GP scratch for memory-to-memory transfers");
    require(rebound.bindings[0].action.source.kind == NativeRegisterLocationKind::HomeSlot &&
            rebound.bindings[0].action.source.spill_slot == 1U &&
            rebound.bindings[0].action.destination.kind == NativeRegisterLocationKind::HomeSlot &&
            rebound.bindings[0].action.destination.spill_slot == 2U &&
            rebound.bindings[1].action.source.spill_slot == 2U &&
            rebound.bindings[1].action.destination.spill_slot == 1U,
        "G1 incorrectly equated N111 local HomeSlot 1 with N112 Flow HomeSlot 0");
    require(rebound.frame_locations.home_slot_offsets.at(1U) == -16 &&
            rebound.frame_locations.home_slot_offsets.at(2U) == -24 &&
            fixture.bindings[0].action.slot_index == 0U && rebound.bindings[0].action.slot_index == 0U,
        "G1 did not preserve the original N112 slot identity while rebasing physical frame keys");
}

G1Fixture make_g1_definition_use_fixture() {
    G1Fixture fixture;
    fixture.function.entry_block = 0U;
    fixture.function.values.push_back(NativeRegisterValueInfo{13U, NativeRegisterClass::GeneralPurpose});
    NativeRegisterBasicBlock block;
    block.id = 2U;
    auto write = flow_instruction(NativeRegisterInstructionOriginKind::FlowWrite, 0U);
    write.flow_block_id = 2U;
    write.definitions.push_back(13U);
    auto read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 1U);
    read.flow_block_id = 2U;
    read.uses.push_back(13U);
    block.instructions = {write, read};
    fixture.function.blocks.push_back(block);

    NativeRegisterFlowOperationLocation write_op;
    write_op.flow_operation_id = write.flow_operation_id;
    write_op.flow_block_id = 2U;
    write_op.flow_operation_index = 0U;
    write_op.native_instruction_index = 0U;
    write_op.before_position = 0U;
    write_op.after_position = 1U;
    write_op.after_locations.push_back(g1_flow_position(13U, NativeRegisterClass::GeneralPurpose,
        NativeRegisterLocationKind::SpillSlot, 0U, 0U));
    NativeRegisterFlowOperationLocation read_op;
    read_op.flow_operation_id = read.flow_operation_id;
    read_op.flow_block_id = 2U;
    read_op.flow_operation_index = 1U;
    read_op.native_instruction_index = 1U;
    read_op.before_position = 1U;
    read_op.after_position = 2U;
    read_op.before_locations.push_back(g1_flow_position(13U, NativeRegisterClass::GeneralPurpose,
        NativeRegisterLocationKind::SpillSlot, 0U, 0U));
    fixture.flow_plan.operations = {write_op, read_op};

    NativeRegisterLocation temporary = make_location(NativeRegisterLocationKind::Temporary);
    NativeRegisterLocation spill = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 0U);
    fixture.flow_plan.actions.push_back(NativeRegisterFlowSegmentActionBinding{
        NativeSpillReloadAction{NativeSpillReloadActionKind::StoreAfterDefinition, 13U, 0U, 0U, 0U,
            temporary, spill}, write.flow_operation_id, 2U, 0U,
        NativeRegisterInstructionOriginKind::FlowWrite, NativeRegisterClass::GeneralPurpose,
        NativeRegisterFlowActionPlacement::AfterOperation});
    fixture.flow_plan.actions.push_back(NativeRegisterFlowSegmentActionBinding{
        NativeSpillReloadAction{NativeSpillReloadActionKind::ReloadBeforeUse, 13U, 0U, 1U, 0U,
            spill, temporary}, read.flow_operation_id, 2U, 1U,
        NativeRegisterInstructionOriginKind::FlowRead, NativeRegisterClass::GeneralPurpose,
        NativeRegisterFlowActionPlacement::BeforeOperation});

    for (const auto& instruction : block.instructions) {
        X64FlowEmitterAnchor anchor;
        anchor.function_index = 0U;
        anchor.flow_operation_id = instruction.flow_operation_id;
        anchor.flow_block_id = 2U;
        anchor.flow_operation_index = instruction.flow_operation_index;
        anchor.origin_kind = instruction.origin_kind;
        anchor.machine_code_before_offset = instruction.flow_operation_index == 0U ? 4U : 12U;
        anchor.machine_code_offset = instruction.flow_operation_index == 0U ? 10U : 18U;
        anchor.function_code_size = 24U;
        anchor.emitted_operand_kind = instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowWrite
            ? X64FlowEmitterAnchor::OperandKind::ValueWrite
            : X64FlowEmitterAnchor::OperandKind::ValueRead;
        anchor.emitted_operand_value_id = 13U;
        anchor.emitted_operand_location = make_location(NativeRegisterLocationKind::Register, 12U);
        fixture.anchors.push_back(anchor);
    }
    NativeX64MachineValueLocation machine_value;
    machine_value.value_id = 13U;
    machine_value.register_class = NativeRegisterClass::GeneralPurpose;
    machine_value.active_location = make_location(NativeRegisterLocationKind::Register, 12U);
    machine_value.canonical_home = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 0U);
    fixture.machine_locations.succeeded = true;
    fixture.machine_locations.values.emplace(13U, machine_value);
    fixture.flow_frame.spill_slot_offsets.emplace(0U, -24);
    fixture.flow_plan.succeeded = true;
    fixture.bindings.push_back(g1_bound_action(write.flow_operation_id, 13U,
        NativeSpillReloadActionKind::StoreAfterDefinition, 0U, NativeRegisterClass::GeneralPurpose,
        temporary, spill, NativeSpillReloadPlacement::AfterOperation, 0U));
    fixture.bindings.push_back(g1_bound_action(read.flow_operation_id, 13U,
        NativeSpillReloadActionKind::ReloadBeforeUse, 1U, NativeRegisterClass::GeneralPurpose,
        spill, temporary, NativeSpillReloadPlacement::BeforeOperation, 0U));
    fixture.emitter_spill_slots = 2U;
    fixture.emitter_frame.spill_slot_offsets = {{0U, -8}, {1U, -16}};
    return fixture;
}

void test_g1_definition_and_use_rebind_exact_emitter_operand_and_spill_namespace() {
    const auto fixture = make_g1_definition_use_fixture();
    const auto rebound = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, fixture.emitter_spill_slots, fixture.emitter_home_slots,
        fixture.local_home_slots, {}, {});
    require(rebound.succeeded && rebound.rebound_actions == 2U &&
            rebound.flow_spill_slots_rebased == 1U,
        "G1 did not rebind both definition/use actions and reserve the Flow spill namespace");
    require(rebound.bindings[0].action.source.kind == NativeRegisterLocationKind::Register &&
            rebound.bindings[0].action.source.register_index == 12U &&
            rebound.bindings[0].action.destination.kind == NativeRegisterLocationKind::SpillSlot &&
            rebound.bindings[0].action.destination.spill_slot == 2U &&
            rebound.bindings[1].action.source.spill_slot == 2U &&
            rebound.bindings[1].action.destination.register_index == 12U &&
            rebound.frame_locations.spill_slot_offsets.at(2U) == -24,
        "G1 did not replace abstract N112 temporaries with the actual N111 emitter operand location");
}

void test_g1_rejects_missing_or_mismatched_emitter_operand_anchor() {
    auto fixture = make_g1_definition_use_fixture();
    fixture.anchors.pop_back();
    const auto missing = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, fixture.emitter_spill_slots, fixture.emitter_home_slots,
        fixture.local_home_slots, {}, {});
    require(!missing.succeeded && !missing.diagnostics.empty(),
        "G1 accepted an action whose FlowRead had no exact emitted operand anchor");

    fixture = make_g1_definition_use_fixture();
    fixture.anchors[0].emitted_operand_value_id = 99U;
    const auto mismatch = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, fixture.emitter_spill_slots, fixture.emitter_home_slots,
        fixture.local_home_slots, {}, {});
    require(!mismatch.succeeded && !mismatch.diagnostics.empty(),
        "G1 accepted a FlowWrite anchor whose emitted operand value ID disagreed with the Action");
}

void test_g1_xmm_memory_to_memory_uses_class_correct_scratch() {
    auto fixture = make_g1_call_rebinding_fixture(NativeRegisterClass::FloatingPoint, 3U, 2U);
    const auto rebound = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, 0U, 0U, fixture.local_home_slots, {}, {});
    require(rebound.succeeded && rebound.memory_to_memory_actions == 2U &&
            rebound.xmm_cycle_temporary_required && !rebound.gp_cycle_temporary_required &&
            rebound.xmm_cycle_temporary.kind == NativeRegisterLocationKind::Temporary &&
            rebound.xmm_cycle_temporary.register_index >= 1U &&
            rebound.xmm_cycle_temporary.register_index <= 5U,
        "G1 failed to select a class-correct XMM scratch for memory-to-memory Flow transfers");
}


void test_g1_selects_independent_gp_scratch_per_action_boundary() {
    auto fixture = make_g1_call_rebinding_fixture();
    fixture.function.values = {
        NativeRegisterValueInfo{7U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{8U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{9U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{10U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{11U, NativeRegisterClass::GeneralPurpose}};

    for (const auto id : {8U, 9U, 10U, 11U}) {
        fixture.machine_locations.values.emplace(id, NativeX64MachineValueLocation{
            id, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)), {}, false});
    }

    auto& operation = fixture.flow_plan.operations.front();
    for (const auto id : {8U, 9U, 10U})
        operation.before_locations.push_back(g1_flow_position(id, NativeRegisterClass::GeneralPurpose,
            NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)));
    for (const auto id : {9U, 10U, 11U})
        operation.after_locations.push_back(g1_flow_position(id, NativeRegisterClass::GeneralPurpose,
            NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)));

    NativeX64ExpressionRegisterLiveness exact_expression_liveness;
    std::map<std::uint64_t, NativeX64ExpressionRegisterLiveness> expression_liveness;
    expression_liveness.emplace(operation.flow_operation_id, exact_expression_liveness);

    // Every candidate register is used by some expression in the function,
    // but each exact insertion boundary still has one dead register.
    const auto rebound = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, 0U, 0U, fixture.local_home_slots,
        {8U, 9U, 10U, 11U}, {}, expression_liveness);

    require(rebound.succeeded && rebound.bindings.size() == 2U &&
            rebound.bindings[0].cycle_temporary_assigned &&
            rebound.bindings[1].cycle_temporary_assigned &&
            rebound.bindings[0].cycle_temporary.register_index == 11U &&
            rebound.bindings[1].cycle_temporary.register_index == 8U,
        "G1 incorrectly required one GP scratch register to be free at both call boundaries and across the whole function");
}

void test_g1_rejects_when_both_gp_and_xmm_scratch_banks_are_live() {
    auto fixture = make_g1_call_rebinding_fixture();
    fixture.machine_locations.values.at(7U).active_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 1U);
    fixture.function.values = {
        NativeRegisterValueInfo{7U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{8U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{9U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{10U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{11U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{16U, NativeRegisterClass::FloatingPoint},
        NativeRegisterValueInfo{17U, NativeRegisterClass::FloatingPoint},
        NativeRegisterValueInfo{18U, NativeRegisterClass::FloatingPoint},
        NativeRegisterValueInfo{19U, NativeRegisterClass::FloatingPoint},
        NativeRegisterValueInfo{20U, NativeRegisterClass::FloatingPoint}};
    for (const auto id : {8U, 9U, 10U, 11U}) {
        fixture.machine_locations.values.emplace(id, NativeX64MachineValueLocation{
            id, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)), {}, false});
    }
    for (std::uint32_t i = 0U; i < 5U; ++i) {
        const std::uint32_t value_id = 16U + i;
        const std::uint8_t reg = static_cast<std::uint8_t>(1U + i);
        fixture.machine_locations.values.emplace(value_id, NativeX64MachineValueLocation{
            value_id, NativeRegisterClass::FloatingPoint,
            make_location(NativeRegisterLocationKind::Register, reg), {}, false});
    }
    auto& operation = fixture.flow_plan.operations.front();
    for (const auto id : {8U, 9U, 10U, 11U}) {
        operation.before_locations.push_back(g1_flow_position(id, NativeRegisterClass::GeneralPurpose,
            NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)));
        operation.after_locations.push_back(g1_flow_position(id, NativeRegisterClass::GeneralPurpose,
            NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)));
    }
    for (std::uint32_t i = 0U; i < 5U; ++i) {
        const std::uint32_t value_id = 16U + i;
        const std::uint8_t reg = static_cast<std::uint8_t>(1U + i);
        operation.before_locations.push_back(g1_flow_position(value_id, NativeRegisterClass::FloatingPoint,
            NativeRegisterLocationKind::Register, reg));
        operation.after_locations.push_back(g1_flow_position(value_id, NativeRegisterClass::FloatingPoint,
            NativeRegisterLocationKind::Register, reg));
    }
    const auto rejected = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, 0U, 0U, fixture.local_home_slots, {}, {});
    require(!rejected.succeeded && !rejected.diagnostics.empty() &&
            rejected.diagnostics.front().code == "N113G0023",
        "G1 selected a GP or XMM scratch register that is live in N112/N111 at the affected call boundary");
}

void test_g1_gp_memory_action_uses_dead_xmm_when_all_gp_registers_are_live() {
    auto fixture = make_g1_call_rebinding_fixture();
    fixture.function.values = {
        NativeRegisterValueInfo{7U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{8U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{9U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{10U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{11U, NativeRegisterClass::GeneralPurpose}};
    for (const auto id : {8U, 9U, 10U, 11U}) {
        fixture.machine_locations.values.emplace(id, NativeX64MachineValueLocation{
            id, NativeRegisterClass::GeneralPurpose,
            make_location(NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)), {}, false});
    }
    auto& operation = fixture.flow_plan.operations.front();
    for (const auto id : {8U, 9U, 10U, 11U}) {
        operation.before_locations.push_back(g1_flow_position(id, NativeRegisterClass::GeneralPurpose,
            NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)));
        operation.after_locations.push_back(g1_flow_position(id, NativeRegisterClass::GeneralPurpose,
            NativeRegisterLocationKind::Register, static_cast<std::uint8_t>(id)));
    }
    NativeX64ExpressionRegisterLiveness snapshot;
    snapshot.xmm_registers = {1U}; // XMM1 is live for an enclosing expression.
    std::map<std::uint64_t, NativeX64ExpressionRegisterLiveness> snapshots;
    snapshots.emplace(operation.flow_operation_id, snapshot);

    const auto rebound = rebind_native_x64_flow_actions_to_emitter_locations(
        fixture.function, fixture.flow_plan, fixture.bindings, fixture.machine_locations,
        fixture.anchors, fixture.emitter_frame, fixture.local_home_offsets,
        fixture.flow_frame, 0U, 0U, fixture.local_home_slots,
        {8U, 9U, 10U, 11U}, {}, snapshots);
    require(rebound.succeeded && rebound.bindings.size() == 2U &&
            rebound.gp_cycle_temporary_required == false && rebound.xmm_cycle_temporary_required &&
            rebound.bindings[0].cycle_temporary_assigned && rebound.bindings[1].cycle_temporary_assigned &&
            rebound.bindings[0].cycle_temporary_register_class == NativeRegisterClass::FloatingPoint &&
            rebound.bindings[1].cycle_temporary_register_class == NativeRegisterClass::FloatingPoint &&
            rebound.bindings[0].cycle_temporary.register_index == 2U &&
            rebound.bindings[1].cycle_temporary.register_index == 2U &&
            rebound.xmm_cycle_temporary.register_index == 2U,
        "G1 did not fall back to a dead volatile XMM register for GP memory-to-memory actions");
}

X64CodegenResult compile_with_n113_g1_rebinding(const std::string& source, bool enabled) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G1 FunctionEmitter fixture did not parse");
    X64CodegenOptions options{"n113-g1-emitter-rebinding", make_coff_x86_64_object_target()};
    options.plan_n113_emitter_action_rebinding = enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G1 FunctionEmitter planning failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

void test_g1_real_emitter_rebinding_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g1_rebinding(source, false);
    const auto planned = compile_with_n113_g1_rebinding(source, true);
    require(baseline.stats.native_n113_emitter_actions_rebound == 0U &&
            baseline.stats.native_n113_emitter_memory_to_memory_actions == 0U,
        "N113-G1 planner must remain disabled by default");
    require(planned.stats.native_n113_emitter_actions_rebound > 0U &&
            planned.stats.native_n113_emitter_actions_rebound ==
                planned.stats.native_n113_flow_actions_resolved,
        "N113-G1 FunctionEmitter did not rebind every resolved action exactly once");
    require(baseline.object_bytes == planned.object_bytes,
        "N113-G1 planner-only mode changed serialized COFF bytes");
}


void test_g2_insertion_allows_same_value_action_at_distinct_operations() {
    std::vector<std::uint8_t> code(40U, 0x90U);
    const std::uint64_t operation_one = (std::uint64_t{1U} << 32U) | 1U;
    const std::uint64_t operation_two = (std::uint64_t{1U} << 32U) | 2U;
    const auto make_reload = [](std::uint64_t operation_id) {
        NativeBoundSpillReloadAction binding;
        binding.flow_operation_id = operation_id;
        binding.origin_kind = NativeRegisterInstructionOriginKind::FlowRead;
        binding.register_class = NativeRegisterClass::GeneralPurpose;
        binding.placement = NativeSpillReloadPlacement::BeforeOperation;
        binding.action.kind = NativeSpillReloadActionKind::ReloadBeforeUse;
        binding.action.block_id = 0U;
        binding.action.instruction_index = 0U;
        binding.action.value_id = 7U;
        binding.action.slot_index = 0U;
        binding.action.source = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 0U);
        binding.action.destination = make_location(NativeRegisterLocationKind::Register, 8U);
        return binding;
    };
    const auto make_anchor = [](std::uint64_t operation_id, std::uint32_t before, std::uint32_t after) {
        X64FlowEmitterAnchor anchor;
        anchor.function_index = 0U;
        anchor.flow_operation_id = operation_id;
        anchor.flow_block_id = 0U;
        anchor.flow_operation_index = static_cast<std::uint32_t>(operation_id & 0xffffffffU);
        anchor.origin_kind = NativeRegisterInstructionOriginKind::FlowRead;
        anchor.machine_code_before_offset = before;
        anchor.machine_code_offset = after;
        anchor.function_code_size = 40U;
        return anchor;
    };
    NativeX64FrameLocationMap frame;
    frame.spill_slot_offsets.emplace(0U, -8);
    NativeRegisterLocation gp_scratch;
    gp_scratch.kind = NativeRegisterLocationKind::Temporary;
    gp_scratch.register_index = 10U;
    NativeRegisterLocation xmm_scratch;
    xmm_scratch.kind = NativeRegisterLocationKind::Temporary;
    xmm_scratch.register_index = 2U;
    const auto inserted = insert_native_x64_spill_reload_actions(
        code, {}, {make_anchor(operation_one, 8U, 12U), make_anchor(operation_two, 20U, 24U)}, {},
        {make_reload(operation_one), make_reload(operation_two)},
        {{7U, NativeRegisterClass::GeneralPurpose}}, gp_scratch, xmm_scratch, frame);
    require(inserted.succeeded && inserted.machine_actions_inserted == 2U &&
            inserted.machine_moves_inserted == 2U && inserted.code.size() == code.size() + 14U,
        "G2 insertion rejected the same Flow value's same-kind actions at distinct operation identities");
}

void test_g2_insertion_uses_each_binding_cycle_temporary() {
    const std::vector<std::uint8_t> code(40U, 0x90U);
    const std::uint64_t store_operation = (std::uint64_t{1U} << 32U) | 1U;
    const std::uint64_t reload_operation = (std::uint64_t{1U} << 32U) | 2U;
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}};
    NativeX64FrameLocationMap frame;
    frame.home_slot_offsets = {{1U, -8}, {2U, -16}};

    NativeRegisterLocation global_gp;
    global_gp.kind = NativeRegisterLocationKind::Temporary;
    global_gp.register_index = 9U; // deliberately different from either binding
    NativeRegisterLocation global_xmm;
    global_xmm.kind = NativeRegisterLocationKind::Temporary;
    global_xmm.register_index = 2U;

    NativeBoundSpillReloadAction store;
    store.flow_operation_id = store_operation;
    store.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
    store.register_class = NativeRegisterClass::GeneralPurpose;
    store.placement = NativeSpillReloadPlacement::BeforeOperation;
    store.action.kind = NativeSpillReloadActionKind::StoreBeforeCall;
    store.action.value_id = 7U;
    store.action.slot_index = 2U;
    store.action.source = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 1U);
    store.action.destination = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U);
    store.cycle_temporary = make_location(NativeRegisterLocationKind::Temporary, 11U);
    store.cycle_temporary_assigned = true;

    NativeBoundSpillReloadAction reload;
    reload.flow_operation_id = reload_operation;
    reload.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
    reload.register_class = NativeRegisterClass::GeneralPurpose;
    reload.placement = NativeSpillReloadPlacement::AfterOperation;
    reload.action.kind = NativeSpillReloadActionKind::ReloadAfterCall;
    reload.action.value_id = 7U;
    reload.action.slot_index = 2U;
    reload.action.source = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U);
    reload.action.destination = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 1U);
    reload.cycle_temporary = make_location(NativeRegisterLocationKind::Temporary, 8U);
    reload.cycle_temporary_assigned = true;

    X64FlowEmitterAnchor store_anchor;
    store_anchor.function_index = 0U;
    store_anchor.flow_operation_id = store_operation;
    store_anchor.flow_block_id = 0U;
    store_anchor.flow_operation_index = 1U;
    store_anchor.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
    store_anchor.machine_code_offset = 16U;
    store_anchor.machine_code_before_offset = 8U;
    store_anchor.machine_code_after_call_offset = 12U;
    store_anchor.function_code_size = static_cast<std::uint32_t>(code.size());

    X64FlowEmitterAnchor reload_anchor;
    reload_anchor.function_index = 0U;
    reload_anchor.flow_operation_id = reload_operation;
    reload_anchor.flow_block_id = 0U;
    reload_anchor.flow_operation_index = 2U;
    reload_anchor.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
    reload_anchor.machine_code_offset = 36U;
    reload_anchor.machine_code_before_offset = 28U;
    reload_anchor.machine_code_after_call_offset = 32U;
    reload_anchor.function_code_size = static_cast<std::uint32_t>(code.size());

    const auto expected_store = encode_native_x64_parallel_copy_schedule(
        {{7U, store.action.source, store.action.destination, false}}, classes,
        make_location(NativeRegisterLocationKind::Temporary, 11U), frame);
    const auto expected_reload = encode_native_x64_parallel_copy_schedule(
        {{7U, reload.action.source, reload.action.destination, false}}, classes,
        make_location(NativeRegisterLocationKind::Temporary, 8U), frame);
    require(expected_store.succeeded && expected_reload.succeeded &&
            expected_store.machine_moves_emitted == 2U && expected_reload.machine_moves_emitted == 2U,
        "G2 per-binding scratch fixture could not create the two expected memory transfers");

    const auto inserted = insert_native_x64_spill_reload_actions(
        code, {}, {store_anchor, reload_anchor}, {}, {store, reload}, classes,
        global_gp, global_xmm, frame);
    require(inserted.succeeded && inserted.machine_actions_inserted == 2U &&
            inserted.machine_moves_inserted == 4U &&
            inserted.offset_adjustments.size() == 2U,
        "G2 did not insert both operations with their bound per-action scratch registers");

    std::vector<std::uint8_t> expected_code;
    expected_code.insert(expected_code.end(), code.begin(), code.begin() + 8);
    expected_code.insert(expected_code.end(), expected_store.bytes.begin(), expected_store.bytes.end());
    expected_code.insert(expected_code.end(), code.begin() + 8, code.begin() + 32);
    expected_code.insert(expected_code.end(), expected_reload.bytes.begin(), expected_reload.bytes.end());
    expected_code.insert(expected_code.end(), code.begin() + 32, code.end());
    require(inserted.code == expected_code,
        "G2 encoded an action with the function-wide scratch instead of its exact binding scratch");
}

void test_g2_gp_memory_action_encodes_with_action_local_xmm_movq() {
    const std::vector<std::uint8_t> code(32U, 0x90U);
    const std::uint64_t operation_id = (std::uint64_t{1U} << 32U) | 9U;
    const std::map<std::uint32_t, NativeRegisterClass> classes{{7U, NativeRegisterClass::GeneralPurpose}};
    NativeX64FrameLocationMap frame;
    frame.home_slot_offsets = {{1U, -8}, {2U, -16}};

    NativeRegisterLocation global_gp = make_location(NativeRegisterLocationKind::Temporary, 9U);
    NativeRegisterLocation global_xmm = make_location(NativeRegisterLocationKind::Temporary, 3U);

    NativeBoundSpillReloadAction action;
    action.flow_operation_id = operation_id;
    action.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
    action.register_class = NativeRegisterClass::GeneralPurpose;
    action.placement = NativeSpillReloadPlacement::BeforeOperation;
    action.action.kind = NativeSpillReloadActionKind::StoreBeforeCall;
    action.action.value_id = 7U;
    action.action.slot_index = 2U;
    action.action.source = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 1U);
    action.action.destination = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U);
    action.cycle_temporary = make_location(NativeRegisterLocationKind::Temporary, 2U);
    action.cycle_temporary_assigned = true;
    action.cycle_temporary_register_class = NativeRegisterClass::FloatingPoint;

    X64FlowEmitterAnchor anchor;
    anchor.function_index = 0U;
    anchor.flow_operation_id = operation_id;
    anchor.flow_block_id = 0U;
    anchor.flow_operation_index = 9U;
    anchor.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
    anchor.machine_code_before_offset = 8U;
    anchor.machine_code_offset = 20U;
    anchor.function_code_size = static_cast<std::uint32_t>(code.size());

    const auto inserted = insert_native_x64_spill_reload_actions(
        code, {}, {anchor}, {}, {action}, classes, global_gp, global_xmm, frame);
    const std::vector<std::uint8_t> expected_movq{
        0xF3U, 0x0FU, 0x7EU, 0x95U, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x0FU, 0xD6U, 0x95U, 0xF0U, 0xFFU, 0xFFU, 0xFFU};
    require(inserted.succeeded && inserted.machine_actions_inserted == 1U &&
            inserted.machine_moves_inserted == 2U && inserted.code.size() == code.size() + expected_movq.size(),
        "G2 rejected a GP memory-to-memory action with an action-local XMM expansion temporary");
    require(std::equal(expected_movq.begin(), expected_movq.end(), inserted.code.begin() + 8),
        "G2 did not encode the GP memory transfer as exact MOVQ load/store bytes through XMM2");

    action.cycle_temporary.register_index = 6U;
    const auto invalid = insert_native_x64_spill_reload_actions(
        code, {}, {anchor}, {}, {action}, classes, global_gp, global_xmm, frame);
    require(!invalid.succeeded && !invalid.diagnostics.empty(),
        "G2 accepted a nonvolatile XMM6 expansion temporary for GP memory transfer");
}

X64CodegenResult compile_with_n113_g5_edge_audit(const std::string& source,
                                                   bool edge_audit_enabled,
                                                   bool g2_insertion_enabled = false) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G5 FunctionEmitter edge-audit fixture did not parse");
    X64CodegenOptions options{"n113-g5-emitter-edge-audit", make_coff_x86_64_object_target()};
    options.audit_n113_flow_edge_locations = edge_audit_enabled;
    options.insert_n113_emitter_spill_reload_actions = g2_insertion_enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G5 FunctionEmitter edge audit failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

X64CodegenResult compile_with_n113_g2_insertion(const std::string& source, bool enabled) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G2 FunctionEmitter fixture did not parse");
    X64CodegenOptions options{"n113-g2-emitter-insertion", make_coff_x86_64_object_target()};
    options.insert_n113_emitter_spill_reload_actions = enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G2 FunctionEmitter insertion failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

void test_g2_real_emitter_inserts_rebound_actions_and_rebases_anchors() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g2_insertion(source, false);
    const auto inserted = compile_with_n113_g2_insertion(source, true);
    require(baseline.stats.native_n113_emitter_actions_inserted == 0U &&
            baseline.stats.native_n113_emitter_transfer_bytes_inserted == 0U,
        "N113-G2 insertion must remain disabled by default");
    require(inserted.stats.native_n113_emitter_actions_rebound > 0U &&
            inserted.stats.native_n113_emitter_actions_inserted ==
                inserted.stats.native_n113_emitter_actions_rebound &&
            inserted.stats.native_n113_emitter_machine_moves_inserted >=
                inserted.stats.native_n113_emitter_actions_inserted &&
            inserted.stats.native_n113_emitter_transfer_bytes_inserted > 0U,
        "N113-G2 did not materialize every rebound action as real machine-code transfers");
    require(inserted.object_bytes != baseline.object_bytes,
        "N113-G2 insertion did not change serialized COFF bytes");
    require(!inserted.flow_anchors.empty() && !inserted.flow_branch_anchors.empty(),
        "N113-G2 fixture did not retain operation and CFG branch anchors");
    const auto& object_file = inserted.native_object.object_file;
    require(!object_file.relocations.empty(),
        "N113-G2 fixture did not retain COFF relocations after code insertion");
    for (const auto& relocation : object_file.relocations) {
        const auto section = std::find_if(object_file.sections.begin(), object_file.sections.end(),
            [&relocation](const ObjectSection& item) { return item.id == relocation.section_id; });
        require(section != object_file.sections.end() && relocation.offset <= section->size &&
                relocation.width <= section->size - relocation.offset,
            "N113-G2 left a COFF relocation outside its rebased function section");
    }
    for (const auto& anchor : inserted.flow_anchors) {
        require(anchor.machine_code_offset <= anchor.function_code_size &&
                (anchor.machine_code_before_offset == InvalidNativeFlowOperationIndex ||
                 anchor.machine_code_before_offset <= anchor.function_code_size) &&
                (anchor.machine_code_after_call_offset == InvalidNativeFlowOperationIndex ||
                 anchor.machine_code_after_call_offset <= anchor.function_code_size),
            "N113-G2 operation anchor was not rebased inside the transformed function");
    }
    for (const auto& anchor : inserted.flow_branch_anchors) {
        require(anchor.instruction_offset <= anchor.function_code_size &&
                anchor.target_offset <= anchor.function_code_size &&
                (anchor.displacement_offset == InvalidNativeFlowOperationIndex ||
                 static_cast<std::uint64_t>(anchor.displacement_offset) + 4U <= anchor.function_code_size),
            "N113-G2 CFG branch anchor was not rebased inside the transformed function");
    }
}

std::pair<std::size_t, std::size_t> count_win64_unwind_sections(const ObjectFile& object_file) {
    std::size_t pdata = 0U;
    std::size_t xdata = 0U;
    for (const auto& section : object_file.sections) {
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata;
    }
    return {pdata, xdata};
}

void test_g3_g2_insertion_preserves_win64_unwind_records() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g2_insertion(source, false);
    const auto inserted = compile_with_n113_g2_insertion(source, true);
    const auto baseline_unwind = count_win64_unwind_sections(baseline.native_object.object_file);
    const auto inserted_unwind = count_win64_unwind_sections(inserted.native_object.object_file);
    require(baseline_unwind.first == 4U && baseline_unwind.second == 4U,
        "G3 baseline fixture must emit one Win64 pdata/xdata pair per function");
    require(inserted_unwind == baseline_unwind && inserted_unwind.first == 4U &&
            inserted.stats.win64_unwind_records_emitted == inserted_unwind.first,
        "G2 transfer insertion changed the function unwind-record inventory");
    require(inserted.stats.native_n113_emitter_actions_inserted > 0U &&
            inserted.stats.native_n113_emitter_transfer_bytes_inserted > 0U,
        "G3 fixture did not exercise real inserted transfer bytes");
}

struct G4FlowEdgeFixture {
    NativeRegisterFlowFunction function;
    Fixture analysis;
    NativeRegisterFlowSegmentPlan flow_segments;
    NativeRegisterHomePhiPlan home_phi;
    NativeX64MachineLocationPlan emitter_locations;
    NativeX64FrameLocationMap flow_frame;
};

NativeX64FlowEdgeLocationAuditPlan audit_g4_fixture(const G4FlowEdgeFixture& fixture);
std::vector<X64FlowBranchAnchor> make_g6_branch_anchors(
    const NativeX64FlowEdgeLocationAuditPlan& audit);

G4FlowEdgeFixture make_g4_flow_edge_fixture() {
    G4FlowEdgeFixture fixture;
    fixture.function.function_name = "g4_diamond";
    fixture.function.register_ir.entry_block = 0U;
    fixture.function.register_ir.values.push_back(
        NativeRegisterValueInfo{7U, NativeRegisterClass::GeneralPurpose});

    NativeRegisterBasicBlock entry;
    entry.id = 0U;
    entry.successors = {1U, 2U};
    auto entry_definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 0U);
    entry_definition.flow_block_id = 0U;
    entry_definition.definitions = {7U};
    entry.instructions.push_back(entry_definition);

    NativeRegisterBasicBlock alternate;
    alternate.id = 1U;
    alternate.predecessors = {0U};
    alternate.successors = {2U};
    auto alternate_definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowWrite, 1U);
    alternate_definition.flow_block_id = 1U;
    alternate_definition.flow_operation_id = (std::uint64_t{2U} << 32U) | 2U;
    alternate_definition.definitions = {7U};
    alternate.instructions.push_back(alternate_definition);

    NativeRegisterBasicBlock merge;
    merge.id = 2U;
    merge.predecessors = {0U, 1U};
    auto merge_read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 2U);
    merge_read.flow_block_id = 2U;
    merge_read.flow_operation_id = (std::uint64_t{3U} << 32U) | 3U;
    merge_read.uses = {7U};
    merge.instructions.push_back(merge_read);

    fixture.function.register_ir.blocks = {entry, alternate, merge};
    fixture.function.edges = {
        FlowEdge{1U, 0U, 1U, FlowEdgeKind::IfTrue},
        FlowEdge{2U, 0U, 2U, FlowEdgeKind::IfFalse},
        FlowEdge{3U, 1U, 2U, FlowEdgeKind::IfThenMerge}
    };

    fixture.analysis = build_fixture(fixture.function.register_ir);
    if (!(fixture.analysis.liveness.succeeded && fixture.analysis.liveness.converged &&
          fixture.analysis.intervals.succeeded && fixture.analysis.segments.succeeded &&
          fixture.analysis.allocation.succeeded)) {
        std::string detail = "G4 diamond fixture could not build a valid N112 allocation";
        const auto append = [&detail](const std::vector<NativeRegisterIRDiagnostic>& diagnostics) {
            if (!diagnostics.empty()) detail += ": " + diagnostics.front().code + " " + diagnostics.front().message;
        };
        append(fixture.analysis.liveness.diagnostics);
        append(fixture.analysis.intervals.diagnostics);
        append(fixture.analysis.segments.diagnostics);
        append(fixture.analysis.allocation.diagnostics);
        throw std::runtime_error(detail);
    }
    fixture.flow_segments = build_plan(fixture.analysis);
    require(fixture.flow_segments.succeeded,
        "G4 diamond fixture could not build N112 block-local segment locations");

    // Force distinct legal N112 locations at the two predecessor exits and
    // the merge entry. The N111 emitter still owns one authoritative location.
    for (auto& segment : fixture.flow_segments.segments) {
        if (segment.segment.value_id != 7U) continue;
        segment.location.kind = NativeRegisterLocationKind::Register;
        segment.location.register_index = segment.segment.block_id == 0U ? 8U :
            (segment.segment.block_id == 1U ? 9U : 10U);
    }

    fixture.home_phi.succeeded = true;
    fixture.home_phi.home_aliases = {
        NativeRegisterHomePhiAlias{100U, 7U},
        NativeRegisterHomePhiAlias{101U, 7U},
        NativeRegisterHomePhiAlias{102U, 7U}
    };
    fixture.home_phi.phis.push_back(NativeRegisterPhiNode{
        2U, 100U, {NativeRegisterPhiIncoming{0U, 101U}, NativeRegisterPhiIncoming{1U, 102U}}});

    fixture.emitter_locations.succeeded = true;
    NativeX64MachineValueLocation emitter_value;
    emitter_value.value_id = 7U;
    emitter_value.register_class = NativeRegisterClass::GeneralPurpose;
    emitter_value.active_location = make_location(NativeRegisterLocationKind::Register, 12U);
    emitter_value.canonical_home = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 0U);
    fixture.emitter_locations.values.emplace(7U, emitter_value);
    return fixture;
}

G4FlowEdgeFixture make_g4_flow_edge_fixture_with_live_scratch_occupant() {
    auto fixture = make_g4_flow_edge_fixture();
    fixture.function.register_ir.values.push_back(
        NativeRegisterValueInfo{8U, NativeRegisterClass::GeneralPurpose});

    auto& entry = fixture.function.register_ir.blocks[0U];
    auto extra_definition = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 1U);
    extra_definition.flow_block_id = 0U;
    extra_definition.definitions = {8U};
    entry.instructions.push_back(extra_definition);

    auto& merge = fixture.function.register_ir.blocks[2U];
    auto extra_read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 3U);
    extra_read.flow_block_id = 2U;
    extra_read.flow_operation_id = (std::uint64_t{3U} << 32U) | 4U;
    extra_read.definitions.clear();
    extra_read.uses = {8U};
    merge.instructions.push_back(extra_read);

    fixture.analysis = build_fixture(fixture.function.register_ir);
    require(fixture.analysis.liveness.succeeded && fixture.analysis.liveness.converged &&
            fixture.analysis.intervals.succeeded && fixture.analysis.segments.succeeded &&
            fixture.analysis.allocation.succeeded,
        "G8 scratch-occupant fixture could not build a valid Flow allocation");
    fixture.flow_segments = build_plan(fixture.analysis);
    require(fixture.flow_segments.succeeded,
        "G8 scratch-occupant fixture could not build its N112 segment plan");

    for (auto& segment : fixture.flow_segments.segments) {
        if (segment.segment.value_id == 7U) {
            segment.location.kind = segment.segment.block_id == 0U
                ? NativeRegisterLocationKind::SpillSlot
                : NativeRegisterLocationKind::HomeSlot;
            segment.location.spill_slot = segment.segment.block_id == 0U
                ? 0U : (segment.segment.block_id == 1U ? 0U : 1U);
        } else if (segment.segment.value_id == 8U) {
            segment.location.kind = NativeRegisterLocationKind::Register;
            segment.location.register_index = 10U;
        }
    }
    fixture.flow_frame.spill_slot_offsets = {{0U, -16}};
    fixture.flow_frame.home_slot_offsets = {{0U, -24}, {1U, -32}};

    NativeX64MachineValueLocation extra_emitter_value;
    extra_emitter_value.value_id = 8U;
    extra_emitter_value.register_class = NativeRegisterClass::GeneralPurpose;
    extra_emitter_value.active_location = make_location(NativeRegisterLocationKind::Register, 13U);
    extra_emitter_value.canonical_home = make_location(NativeRegisterLocationKind::HomeSlot, 1U, 0U);
    fixture.emitter_locations.values.emplace(8U, extra_emitter_value);
    return fixture;
}

std::vector<X64FlowEmitterAnchor> make_g8_operand_anchors(
    const G4FlowEdgeFixture& fixture) {
    std::vector<X64FlowEmitterAnchor> anchors;
    std::uint32_t offset = 1U;
    for (const auto& block : fixture.function.register_ir.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            X64FlowEmitterAnchor anchor;
            anchor.function_index = 0U;
            anchor.flow_operation_id = instruction.flow_operation_id;
            anchor.flow_block_id = instruction.flow_block_id;
            anchor.flow_operation_index = instruction.flow_operation_index;
            anchor.origin_kind = instruction.origin_kind;
            anchor.source_span = instruction.source_span;
            anchor.machine_code_before_offset = offset;
            anchor.machine_code_offset = offset + 1U;
            anchor.function_code_size = 256U;
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowRead &&
                instruction.uses.size() == 1U) {
                anchor.emitted_operand_kind = X64FlowEmitterAnchor::OperandKind::ValueRead;
                anchor.emitted_operand_value_id = instruction.uses.front();
            } else if (instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowWrite &&
                       instruction.definitions.size() == 1U) {
                anchor.emitted_operand_kind = X64FlowEmitterAnchor::OperandKind::ValueWrite;
                anchor.emitted_operand_value_id = instruction.definitions.front();
            } else if (instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowDeclare &&
                       instruction.definitions.size() == 1U) {
                anchor.emitted_operand_kind = X64FlowEmitterAnchor::OperandKind::ValueDeclare;
                anchor.emitted_operand_value_id = instruction.definitions.front();
            }
            if (anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None) {
                const auto machine_value = fixture.emitter_locations.values.find(anchor.emitted_operand_value_id);
                if (machine_value == fixture.emitter_locations.values.end())
                    throw std::runtime_error("G8 test fixture operand is missing an N111 Value location");
                anchor.emitted_operand_location = machine_value->second.active_location;
            }
            anchors.push_back(anchor);
            offset += 3U;
        }
    }
    return anchors;
}

struct G8TestArtifacts {
    NativeX64FlowEdgeLocationAuditPlan edge_audit;
    NativeX64FlowEdgeTransferCandidatePlan candidates;
    NativeX64FlowEdgeTransferSchedulePlan schedule;
    std::vector<X64FlowBranchAnchor> branch_anchors;
    std::vector<X64FlowEmitterAnchor> operation_anchors;
    std::map<std::uint32_t, std::int32_t> emitter_local_home_offsets;
};

G8TestArtifacts make_g8_test_artifacts(const G4FlowEdgeFixture& fixture,
                                       NativeRegisterLocation gp_temporary,
                                       NativeRegisterLocation xmm_temporary) {
    G8TestArtifacts artifacts;
    artifacts.edge_audit = audit_g4_fixture(fixture);
    require(artifacts.edge_audit.succeeded, "G8 fixture did not produce a valid G4 edge audit");
    artifacts.branch_anchors = make_g6_branch_anchors(artifacts.edge_audit);
    std::map<std::uint32_t, NativeRegisterClass> classes;
    for (const auto& value : fixture.function.register_ir.values)
        classes.emplace(value.value_id, value.register_class);
    artifacts.candidates = plan_native_x64_flow_edge_transfer_candidates(
        artifacts.edge_audit, artifacts.branch_anchors, classes);
    require(artifacts.candidates.succeeded, "G8 fixture did not produce valid G6 edge candidates");
    artifacts.schedule = schedule_native_x64_flow_edge_transfer_candidates(
        artifacts.candidates, classes, gp_temporary, xmm_temporary);
    require(artifacts.schedule.succeeded, "G8 fixture did not produce a valid G7 parallel-copy schedule");
    artifacts.operation_anchors = make_g8_operand_anchors(fixture);
    artifacts.emitter_local_home_offsets = {{0U, -8}, {1U, -16}};
    return artifacts;
}

X64CodegenResult compile_with_n113_g8_edge_readiness(const std::string& source,
                                                      bool enabled,
                                                      bool g2_insertion_enabled = false) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G8 FunctionEmitter fixture did not parse");
    X64CodegenOptions options{"n113-g8-edge-readiness", make_coff_x86_64_object_target()};
    options.audit_n113_flow_edge_schedule_readiness = enabled;
    options.insert_n113_emitter_spill_reload_actions = g2_insertion_enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G8 FunctionEmitter readiness audit failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

struct G9PerEdgeScratchFixture {
    G4FlowEdgeFixture fixture;
    NativeX64FlowEdgeLocationAuditPlan edge_audit;
    NativeX64FlowEdgeTransferCandidatePlan candidates;
    std::vector<X64FlowBranchAnchor> branch_anchors;
    std::vector<X64FlowEmitterAnchor> operation_anchors;
    std::map<std::uint32_t, std::int32_t> emitter_local_home_offsets;
};

G9PerEdgeScratchFixture make_g9_per_edge_scratch_fixture(
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose) {
    G9PerEdgeScratchFixture artifacts;
    artifacts.fixture = make_g4_flow_edge_fixture();
    artifacts.edge_audit = audit_g4_fixture(artifacts.fixture);
    require(artifacts.edge_audit.succeeded && artifacts.edge_audit.values.size() == 2U,
        "G9 fixture did not produce the two exact diamond-edge ownership records");
    artifacts.branch_anchors = make_g6_branch_anchors(artifacts.edge_audit);

    artifacts.fixture.flow_frame.spill_slot_offsets = {{0U, -16}};
    artifacts.fixture.flow_frame.home_slot_offsets = {{0U, -24}, {1U, -32}};
    for (auto& value : artifacts.edge_audit.values) {
        if (value.value_id == 7U) value.register_class = register_class;
        value.predecessor_flow_location = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 0U);
        value.successor_flow_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U,
            value.edge_id == 2U ? 0U : 1U);
        value.predecessor_uses_frame = true;
        value.successor_uses_frame = true;
        value.predecessor_frame_displacement = -16;
        value.successor_frame_displacement = value.edge_id == 2U ? -24 : -32;
        value.flow_location_changes = true;
    }

    // Two independent live-through values occupy different scratch registers
    // on two different edges. A single function-wide GP scratch cannot satisfy
    // both restrictions: edge 1 blocks R8; edge 2 blocks R9.
    artifacts.fixture.function.register_ir.values.front().register_class = register_class;
    artifacts.fixture.function.register_ir.values.push_back(
        NativeRegisterValueInfo{8U, register_class});
    artifacts.fixture.function.register_ir.values.push_back(
        NativeRegisterValueInfo{9U, register_class});
    const std::uint8_t edge_one_occupant_register =
        register_class == NativeRegisterClass::GeneralPurpose ? 8U : 1U;
    const std::uint8_t edge_two_occupant_register =
        register_class == NativeRegisterClass::GeneralPurpose ? 9U : 2U;
    artifacts.fixture.emitter_locations.values.at(7U).register_class = register_class;
    artifacts.fixture.emitter_locations.values.at(7U).active_location =
        make_location(NativeRegisterLocationKind::Register, 12U);
    artifacts.fixture.emitter_locations.values.emplace(8U, NativeX64MachineValueLocation{
        8U, register_class,
        make_location(NativeRegisterLocationKind::Register, edge_one_occupant_register),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U)});
    artifacts.fixture.emitter_locations.values.emplace(9U, NativeX64MachineValueLocation{
        9U, register_class,
        make_location(NativeRegisterLocationKind::Register, edge_two_occupant_register),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 3U)});
    artifacts.fixture.analysis.liveness.blocks[0U].live_out.push_back(8U);
    artifacts.fixture.analysis.liveness.blocks[1U].live_out.push_back(9U);
    artifacts.fixture.analysis.liveness.blocks[2U].live_in.push_back(8U);
    artifacts.fixture.analysis.liveness.blocks[2U].live_in.push_back(9U);

    NativeX64FlowEdgeValueAudit edge_one_occupant;
    edge_one_occupant.edge_id = 2U;
    edge_one_occupant.predecessor_block_id = 0U;
    edge_one_occupant.successor_block_id = 2U;
    edge_one_occupant.value_id = 8U;
    edge_one_occupant.register_class = register_class;
    edge_one_occupant.predecessor_flow_location = make_location(
        NativeRegisterLocationKind::Register, edge_one_occupant_register);
    edge_one_occupant.successor_flow_location = make_location(
        NativeRegisterLocationKind::Register, edge_one_occupant_register);
    edge_one_occupant.emitter_active_location = make_location(
        NativeRegisterLocationKind::Register, edge_one_occupant_register);
    artifacts.edge_audit.values.push_back(edge_one_occupant);

    NativeX64FlowEdgeValueAudit edge_two_occupant;
    edge_two_occupant.edge_id = 3U;
    edge_two_occupant.predecessor_block_id = 1U;
    edge_two_occupant.successor_block_id = 2U;
    edge_two_occupant.value_id = 9U;
    edge_two_occupant.register_class = register_class;
    edge_two_occupant.predecessor_flow_location = make_location(
        NativeRegisterLocationKind::Register, edge_two_occupant_register);
    edge_two_occupant.successor_flow_location = make_location(
        NativeRegisterLocationKind::Register, edge_two_occupant_register);
    edge_two_occupant.emitter_active_location = make_location(
        NativeRegisterLocationKind::Register, edge_two_occupant_register);
    artifacts.edge_audit.values.push_back(edge_two_occupant);
    artifacts.edge_audit.live_values_audited = 4U;

    artifacts.candidates.succeeded = true;
    artifacts.candidates.ready_for_machine_insertion = false;
    artifacts.candidates.candidate_edges = 2U;
    artifacts.candidates.candidate_copies = 2U;
    for (const auto& value : artifacts.edge_audit.values) {
        if (value.value_id != 7U) continue;
        const auto anchor = std::find_if(artifacts.branch_anchors.begin(), artifacts.branch_anchors.end(),
            [&](const X64FlowBranchAnchor& item) { return item.edge_id == value.edge_id; });
        require(anchor != artifacts.branch_anchors.end(), "G9 fixture lacks an exact branch anchor");
        NativeX64FlowEdgeTransferCandidate candidate;
        candidate.edge_id = value.edge_id;
        candidate.predecessor_block_id = value.predecessor_block_id;
        candidate.successor_block_id = value.successor_block_id;
        candidate.branch_anchor = *anchor;
        candidate.copy = NativeRegisterParallelCopy{
            value.value_id, value.predecessor_flow_location, value.successor_flow_location};
        candidate.emitter_active_location = value.emitter_active_location;
        artifacts.candidates.candidates.push_back(std::move(candidate));
    }
    require(artifacts.candidates.candidates.size() == 2U,
        "G9 fixture did not create one memory-to-memory candidate per exact edge");
    artifacts.operation_anchors = make_g8_operand_anchors(artifacts.fixture);
    artifacts.emitter_local_home_offsets = {{0U, -8}, {1U, -16}, {2U, -24}, {3U, -32}};
    return artifacts;
}

X64CodegenResult compile_with_n113_g9_per_edge_scratch(const std::string& source,
                                                       bool enabled,
                                                       bool g2_insertion_enabled = false) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G9 FunctionEmitter fixture did not parse");
    X64CodegenOptions options{"n113-g9-per-edge-scratch", make_coff_x86_64_object_target()};
    options.plan_n113_flow_edge_scratch_per_edge = enabled;
    options.insert_n113_emitter_spill_reload_actions = g2_insertion_enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G9 FunctionEmitter per-edge scratch planning failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

X64CodegenResult compile_with_n113_g11_edge_eligibility(const std::string& source,
                                                            bool enabled,
                                                            bool g2_insertion_enabled = false) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G11 FunctionEmitter fixture did not parse");
    X64CodegenOptions options{"n113-g11-edge-bundle-eligibility", make_coff_x86_64_object_target()};
    options.audit_n113_flow_edge_bundle_eligibility = enabled;
    options.insert_n113_emitter_spill_reload_actions = g2_insertion_enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G11 FunctionEmitter eligibility audit failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

X64CodegenResult compile_with_n113_g10_operand_ownership(const std::string& source,
                                                              bool enabled,
                                                              bool g2_insertion_enabled = false,
                                                              bool g9_enabled = false) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G10 FunctionEmitter operand-ownership fixture did not parse");
    X64CodegenOptions options{"n113-g10-operation-operand-ownership", make_coff_x86_64_object_target()};
    options.audit_n113_flow_operation_operand_ownership = enabled;
    options.insert_n113_emitter_spill_reload_actions = g2_insertion_enabled;
    options.plan_n113_flow_edge_scratch_per_edge = g9_enabled;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G10 FunctionEmitter operand-ownership audit failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

void set_g10_operation_value_location(NativeRegisterFlowSegmentPlan& plan,
                                      const std::uint32_t value_id,
                                      const NativeRegisterLocation& location) {
    for (auto& operation : plan.operations) {
        for (auto& value : operation.before_locations) {
            if (value.value_id == value_id) value.location = location;
        }
        for (auto& value : operation.after_locations) {
            if (value.value_id == value_id) value.location = location;
        }
    }
}

void test_g9_selects_distinct_gp_scratch_per_exact_edge() {
    const auto artifacts = make_g9_per_edge_scratch_fixture();
    const auto plan = plan_native_x64_flow_edge_schedules_per_edge(
        artifacts.fixture.function, artifacts.fixture.analysis.liveness,
        artifacts.edge_audit, artifacts.candidates, artifacts.fixture.emitter_locations,
        artifacts.emitter_local_home_offsets, artifacts.fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors, {}, {});
    require(plan.succeeded && plan.candidate_edges == 2U && plan.scheduled_edges == 2U &&
            plan.scratch_proven_edges == 2U && plan.scratch_liveness_proven,
        "G9 did not independently prove scratch liveness for both exact CFG edges");
    const auto first = std::find_if(plan.edges.begin(), plan.edges.end(),
        [](const auto& item) { return item.edge_id == 2U; });
    const auto second = std::find_if(plan.edges.begin(), plan.edges.end(),
        [](const auto& item) { return item.edge_id == 3U; });
    const std::string actual = (first == plan.edges.end() || second == plan.edges.end())
        ? "missing edge output"
        : ("edge1 gp=" + std::to_string(first->gp_cycle_temporary.register_index) +
           " proven=" + std::to_string(first->scratch_selection_proven) +
           " required=" + std::to_string(first->readiness.gp_scratch_required) +
           " dead=" + std::to_string(first->readiness.gp_scratch_dead) +
           "; edge2 gp=" + std::to_string(second->gp_cycle_temporary.register_index) +
           " proven=" + std::to_string(second->scratch_selection_proven) +
           " required=" + std::to_string(second->readiness.gp_scratch_required) +
           " dead=" + std::to_string(second->readiness.gp_scratch_dead));
    require(first != plan.edges.end() && second != plan.edges.end() &&
            first->schedule_built && second->schedule_built &&
            first->scratch_selection_proven && second->scratch_selection_proven &&
            first->gp_cycle_temporary.kind == NativeRegisterLocationKind::Temporary &&
            first->gp_cycle_temporary.register_index == 9U &&
            second->gp_cycle_temporary.kind == NativeRegisterLocationKind::Temporary &&
            second->gp_cycle_temporary.register_index == 8U,
        actual.c_str());
    require(!plan.machine_operand_ownership_proven && !plan.machine_insertion_ready,
        "G9 upgraded the known N112/N111 endpoint ownership mismatch to insertion readiness");
}

void test_g9_keeps_each_schedule_bound_to_its_exact_branch_anchor() {
    const auto artifacts = make_g9_per_edge_scratch_fixture();
    const auto plan = plan_native_x64_flow_edge_schedules_per_edge(
        artifacts.fixture.function, artifacts.fixture.analysis.liveness,
        artifacts.edge_audit, artifacts.candidates, artifacts.fixture.emitter_locations,
        artifacts.emitter_local_home_offsets, artifacts.fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors, {}, {});
    require(plan.succeeded && plan.edges.size() == 2U,
        "G9 did not retain one output entry per candidate FlowEdgeId");
    for (const auto& entry : plan.edges) {
        require(entry.schedule_built && entry.schedule.edge_id == entry.edge_id &&
                entry.schedule.bundle.edge_id == entry.edge_id &&
                entry.schedule.branch_anchor.edge_id == entry.edge_id &&
                entry.readiness.edge_id == entry.edge_id &&
                entry.schedule.predecessor_block_id == entry.schedule.branch_anchor.from_block_id &&
                entry.schedule.successor_block_id == entry.schedule.branch_anchor.to_block_id,
            "G9 detached a per-edge schedule, proof or branch anchor from its exact FlowEdgeId");
    }
}

void test_g9_selects_xmm_scratch_independently_from_gp_register_file() {
    const auto artifacts = make_g9_per_edge_scratch_fixture(NativeRegisterClass::FloatingPoint);
    const auto plan = plan_native_x64_flow_edge_schedules_per_edge(
        artifacts.fixture.function, artifacts.fixture.analysis.liveness,
        artifacts.edge_audit, artifacts.candidates, artifacts.fixture.emitter_locations,
        artifacts.emitter_local_home_offsets, artifacts.fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors, {}, {});
    std::string xmm_actual;
    for (const auto& item : plan.edges) {
        if (!xmm_actual.empty()) xmm_actual += "; ";
        xmm_actual += "edge=" + std::to_string(item.edge_id) + " selected=" +
            std::to_string(item.xmm_cycle_temporary.register_index) + " proven=" +
            std::to_string(item.scratch_selection_proven) + " required=" +
            std::to_string(item.readiness.xmm_scratch_required) + " dead=" +
            std::to_string(item.readiness.xmm_scratch_dead);
    }
    require(plan.succeeded && plan.scratch_liveness_proven && plan.edges.size() == 2U,
        xmm_actual.c_str());
    const auto first = std::find_if(plan.edges.begin(), plan.edges.end(),
        [](const auto& item) { return item.edge_id == 2U; });
    const auto second = std::find_if(plan.edges.begin(), plan.edges.end(),
        [](const auto& item) { return item.edge_id == 3U; });
    require(first != plan.edges.end() && second != plan.edges.end() &&
            first->xmm_cycle_temporary.register_index == 2U &&
            second->xmm_cycle_temporary.register_index == 1U &&
            first->gp_cycle_temporary.register_index == 8U &&
            second->gp_cycle_temporary.register_index == 8U,
        "G9 did not select edge-local XMM2/XMM1 while keeping the GP register file independent");
}

void test_g9_fails_closed_when_expression_reserves_every_gp_scratch() {
    const auto artifacts = make_g9_per_edge_scratch_fixture();
    const std::vector<std::uint8_t> all_gp_scratch{8U, 9U, 10U, 11U};
    const auto plan = plan_native_x64_flow_edge_schedules_per_edge(
        artifacts.fixture.function, artifacts.fixture.analysis.liveness,
        artifacts.edge_audit, artifacts.candidates, artifacts.fixture.emitter_locations,
        artifacts.emitter_local_home_offsets, artifacts.fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors, all_gp_scratch, {});
    require(plan.succeeded && plan.scheduled_edges == 2U &&
            plan.scratch_proven_edges == 0U && plan.scratch_unavailable_edges == 2U &&
            !plan.scratch_liveness_proven && !plan.machine_insertion_ready,
        "G9 accepted an edge after every class-correct GP memory-expansion scratch was expression-reserved");
}

void test_g9_rejects_candidate_plan_claiming_machine_readiness() {
    const auto artifacts = make_g9_per_edge_scratch_fixture();
    auto candidates = artifacts.candidates;
    candidates.ready_for_machine_insertion = true;
    const auto plan = plan_native_x64_flow_edge_schedules_per_edge(
        artifacts.fixture.function, artifacts.fixture.analysis.liveness,
        artifacts.edge_audit, candidates, artifacts.fixture.emitter_locations,
        artifacts.emitter_local_home_offsets, artifacts.fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors, {}, {});
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0060",
        "G9 accepted a G6 candidate table that falsely claimed machine insertion readiness");
}

void test_g9_scratch_selection_is_deterministic() {
    const auto artifacts = make_g9_per_edge_scratch_fixture();
    const auto run = [&]() {
        return plan_native_x64_flow_edge_schedules_per_edge(
            artifacts.fixture.function, artifacts.fixture.analysis.liveness,
            artifacts.edge_audit, artifacts.candidates, artifacts.fixture.emitter_locations,
            artifacts.emitter_local_home_offsets, artifacts.fixture.flow_frame,
            artifacts.operation_anchors, artifacts.branch_anchors, {}, {});
    };
    const auto first = run();
    const auto second = run();
    require(first.succeeded && second.succeeded && first.edges.size() == second.edges.size(),
        "G9 deterministic fixture did not produce the same number of edge plans");
    for (std::size_t index = 0U; index < first.edges.size(); ++index) {
        require(first.edges[index].edge_id == second.edges[index].edge_id &&
                first.edges[index].gp_cycle_temporary.register_index ==
                    second.edges[index].gp_cycle_temporary.register_index &&
                first.edges[index].xmm_cycle_temporary.register_index ==
                    second.edges[index].xmm_cycle_temporary.register_index,
            "G9 selected different scratch registers for the same ordered FlowEdgeId");
    }
}

void test_g9_function_emitter_planner_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g9_per_edge_scratch(source, false);
    const auto planned = compile_with_n113_g9_per_edge_scratch(source, true);
    require(baseline.stats.native_n113_flow_edge_per_edge_plans_built == 0U &&
            baseline.stats.native_n113_flow_edge_per_edge_schedules_built == 0U,
        "N113-G9 per-edge scratch planner must remain disabled by default");
    const std::string emitter_counts = "N113-G9 FunctionEmitter counters: plans=" +
        std::to_string(planned.stats.native_n113_flow_edge_per_edge_plans_built) +
        " schedules=" + std::to_string(planned.stats.native_n113_flow_edge_per_edge_schedules_built) +
        " scratch=" + std::to_string(planned.stats.native_n113_flow_edge_per_edge_scratch_proven) +
        " blocked=" + std::to_string(planned.stats.native_n113_flow_edge_per_edge_scratch_blocked) +
        " g7reject=" + std::to_string(planned.stats.native_n113_flow_edge_per_edge_g7_rejections) +
        " proofreject=" + std::to_string(planned.stats.native_n113_flow_edge_per_edge_proof_rejections) +
        " g7diag=" + planned.stats.native_n113_flow_edge_per_edge_first_g7_rejection +
        " proofdiag=" + planned.stats.native_n113_flow_edge_per_edge_first_proof_rejection;
    require(planned.stats.native_n113_flow_edge_per_edge_plans_built > 0U &&
            planned.stats.native_n113_flow_edge_per_edge_schedules_built > 0U &&
            planned.stats.native_n113_flow_edge_per_edge_scratch_proven > 0U,
        emitter_counts.c_str());
    require(planned.stats.native_n113_flow_edge_per_edge_machine_ready_plans == 0U &&
            planned.stats.native_n113_emitter_actions_inserted == 0U &&
            planned.stats.native_n113_flow_frame_spill_slots_reserved == 0U &&
            planned.stats.native_n113_flow_frame_home_slots_reserved == 0U &&
            planned.object_bytes == baseline.object_bytes,
        "N113-G9 read-only planner changed Machine Code, Flow Frame reservation or insertion readiness");
}

void test_g9_per_edge_audit_composes_with_g2_without_changing_moves() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g9_per_edge_scratch(source, false, true);
    const auto g2_plus_g9 = compile_with_n113_g9_per_edge_scratch(source, true, true);
    require(g2_only.stats.native_n113_emitter_actions_inserted > 0U &&
            g2_plus_g9.stats.native_n113_flow_edge_per_edge_plans_built > 0U &&
            g2_plus_g9.stats.native_n113_emitter_actions_inserted ==
                g2_only.stats.native_n113_emitter_actions_inserted &&
            g2_plus_g9.stats.native_n113_emitter_machine_moves_inserted ==
                g2_only.stats.native_n113_emitter_machine_moves_inserted &&
            g2_plus_g9.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_only.stats.native_n113_emitter_transfer_bytes_inserted &&
            g2_plus_g9.object_bytes == g2_only.object_bytes,
        "N113-G9 changed the independently enabled G2 insertion bytes or Machine Move accounting");
}

void test_g10_reports_exact_n112_to_emitter_operand_mismatches() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U,
        make_g8_operand_anchors(fixture));
    require(plan.succeeded && plan.source_operations_audited == 3U &&
            plan.operand_operations_audited == 3U &&
            plan.operand_location_mismatches > 0U && !plan.all_operands_owned,
        "G10 did not report exact operation-level N112/N111 operand mismatches");
    for (const auto& operand : plan.operands) {
        require(operand.flow_operation_id != InvalidNativeFlowOperationId &&
                operand.value_id == 7U &&
                operand.emitter_operand_location.register_index == 12U,
            "G10 ownership evidence lost the stable operation/Value/actual emitter location");
    }
}

void test_g10_proves_each_n112_operation_operand_when_physical_location_matches() {
    auto fixture = make_g4_flow_edge_fixture();
    set_g10_operation_value_location(fixture.flow_segments, 7U,
        make_location(NativeRegisterLocationKind::Register, 12U));
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U,
        make_g8_operand_anchors(fixture));
    require(plan.succeeded && plan.operand_operations_audited == 3U &&
            plan.operand_location_matches == 3U && plan.operand_location_mismatches == 0U &&
            plan.all_operands_owned,
        "G10 failed to prove all exact operation operands after N112 locations were rebound to N111");
}

void test_g10_compares_flow_and_emitter_memory_by_physical_frame_offset() {
    auto fixture = make_g4_flow_edge_fixture();
    auto& value = fixture.emitter_locations.values.at(7U);
    value.active_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U);
    value.canonical_home = value.active_location;
    set_g10_operation_value_location(fixture.flow_segments, 7U,
        make_location(NativeRegisterLocationKind::SpillSlot, 0U));
    fixture.flow_frame.spill_slot_offsets = {{0U, -8}};
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}}, fixture.flow_frame, 0U, make_g8_operand_anchors(fixture));
    require(plan.succeeded && plan.operand_operations_audited == 3U &&
            plan.operand_location_matches == 3U && plan.frame_location_matches == 3U &&
            plan.all_operands_owned,
        "G10 did not equate distinct N112/Emitter slot IDs with the same physical RBP displacement");
}

void test_g10_rejects_same_numbered_slots_with_different_physical_offsets() {
    auto fixture = make_g4_flow_edge_fixture();
    auto& value = fixture.emitter_locations.values.at(7U);
    value.active_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U);
    value.canonical_home = value.active_location;
    set_g10_operation_value_location(fixture.flow_segments, 7U,
        make_location(NativeRegisterLocationKind::SpillSlot, 0U));
    fixture.flow_frame.spill_slot_offsets = {{0U, -16}};
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}}, fixture.flow_frame, 0U, make_g8_operand_anchors(fixture));
    require(plan.succeeded && plan.operand_location_mismatches == 3U &&
            !plan.all_operands_owned && !plan.operands.front().location_matches,
        "G10 equated matching numeric slot IDs despite different physical frame offsets");
}

void test_g10_rejects_duplicate_operation_anchors() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto anchors = make_g8_operand_anchors(fixture);
    anchors.push_back(anchors.front());
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U, anchors);
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113J0006",
        "G10 accepted duplicate operation-anchor identity");
}

void test_g10_rejects_anchor_from_another_function_index() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto anchors = make_g8_operand_anchors(fixture);
    anchors.front().function_index = 1U;
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U, anchors);
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113J0006",
        "G10 accepted an operation anchor belonging to another FunctionEmitter function index");
}

void test_g10_rejects_missing_concrete_operation_anchor() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto anchors = make_g8_operand_anchors(fixture);
    anchors.pop_back();
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U, anchors);
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113J0007",
        "G10 accepted an incomplete per-operation emitter-anchor crosswalk");
}

void test_g10_rejects_missing_n112_operand_position() {
    auto fixture = make_g4_flow_edge_fixture();
    const auto flow_read_id = fixture.function.register_ir.blocks[2U].instructions.front().flow_operation_id;
    auto operation = std::find_if(fixture.flow_segments.operations.begin(),
        fixture.flow_segments.operations.end(), [flow_read_id](const auto& item) {
            return item.flow_operation_id == flow_read_id;
        });
    require(operation != fixture.flow_segments.operations.end(), "G10 fixture is missing its exact FlowRead N112 operation");
    operation->before_locations.erase(std::remove_if(operation->before_locations.begin(),
        operation->before_locations.end(), [](const auto& item) { return item.value_id == 7U; }),
        operation->before_locations.end());
    require(std::none_of(operation->before_locations.begin(), operation->before_locations.end(),
        [](const auto& item) { return item.value_id == 7U; }),
        "G10 test failed to remove the exact FlowRead N112 before-position value");
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U,
        make_g8_operand_anchors(fixture));
    if (plan.succeeded) {
        std::string detail = "G10 unexpectedly succeeded for removed FlowRead N112 before position id=" +
            std::to_string(flow_read_id) + " entries=" + std::to_string(plan.operands.size());
        for (const auto& item : plan.operands) detail += " [" + std::to_string(item.flow_operation_id) +
            ":" + std::to_string(item.value_id) + "]";
        throw std::runtime_error(detail);
    }
    require(std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "N113J0013";
        }), "G10 did not emit the exact missing N112 before-position diagnostic");
}

void test_g10_audits_address_reads_against_the_canonical_home() {
    auto fixture = make_g4_flow_edge_fixture();
    auto& machine_value = fixture.emitter_locations.values.at(7U);
    machine_value.address_taken = true;
    machine_value.active_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U);
    machine_value.canonical_home = machine_value.active_location;
    set_g10_operation_value_location(fixture.flow_segments, 7U,
        make_location(NativeRegisterLocationKind::HomeSlot, 0U));
    fixture.flow_frame.home_slot_offsets = {{0U, -8}};
    auto anchors = make_g8_operand_anchors(fixture);
    for (auto& anchor : anchors) {
        if (anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowRead) {
            anchor.emitted_operand_kind = X64FlowEmitterAnchor::OperandKind::AddressRead;
            anchor.emitted_operand_location = machine_value.active_location;
        }
    }
    const auto plan = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}}, fixture.flow_frame, 0U, anchors);
    require(plan.succeeded && plan.address_operands_audited == 1U &&
            plan.operand_location_matches == 3U && plan.all_operands_owned,
        "G10 failed to preserve address-taken FlowRead ownership at the canonical home");
}

void test_g10_function_emitter_audit_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g10_operand_ownership(source, false);
    const auto audited = compile_with_n113_g10_operand_ownership(source, true);
    require(baseline.stats.native_n113_flow_operation_operand_ownership_plans == 0U &&
            baseline.stats.native_n113_flow_operation_operands_audited == 0U,
        "N113-G10 audit must remain disabled by default");
    require(audited.stats.native_n113_flow_operation_operand_ownership_plans > 0U &&
            audited.stats.native_n113_flow_operation_operands_audited > 0U,
        "N113-G10 did not audit concrete FunctionEmitter operation operands");
    require(audited.object_bytes == baseline.object_bytes &&
            audited.stats.native_n113_emitter_actions_inserted == 0U &&
            audited.stats.native_n113_flow_frame_spill_slots_reserved == 0U &&
            audited.stats.native_n113_flow_frame_home_slots_reserved == 0U,
        "N113-G10 read-only operation crosswalk changed code, inserted transfers or reserved the active Flow frame");
}

void test_g10_crosswalk_composes_with_g2_and_g9_without_changing_moves() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_g9_only = compile_with_n113_g10_operand_ownership(source, false, true, true);
    const auto g2_g9_g10 = compile_with_n113_g10_operand_ownership(source, true, true, true);
    require(g2_g9_only.stats.native_n113_emitter_actions_inserted > 0U &&
            g2_g9_g10.stats.native_n113_flow_operation_operand_ownership_plans > 0U &&
            g2_g9_g10.stats.native_n113_emitter_actions_inserted ==
                g2_g9_only.stats.native_n113_emitter_actions_inserted &&
            g2_g9_g10.stats.native_n113_emitter_machine_moves_inserted ==
                g2_g9_only.stats.native_n113_emitter_machine_moves_inserted &&
            g2_g9_g10.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_g9_only.stats.native_n113_emitter_transfer_bytes_inserted &&
            g2_g9_g10.object_bytes == g2_g9_only.object_bytes,
        "N113-G10 changed G2/G9 output or transfer counts while collecting read-only operand evidence");
}

void test_g8_reports_operand_owner_mismatch_but_proves_dead_scratch() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto artifacts = make_g8_test_artifacts(fixture,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, artifacts.edge_audit, artifacts.schedule,
        fixture.emitter_locations, artifacts.emitter_local_home_offsets, fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U), {}, {});
    require(proof.succeeded && proof.scheduled_edges_audited == 2U &&
            proof.live_edge_values_audited == 2U && proof.operand_anchors_validated == 6U &&
            !proof.machine_operand_ownership_proven && proof.endpoint_ownership_mismatches == 4U &&
            proof.scratch_liveness_proven && !proof.machine_insertion_ready,
        "G8 did not distinguish valid operand-anchor evidence from N112/N111 endpoint ownership mismatch");
}

void test_g8_rejects_missing_final_branch_anchor() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto artifacts = make_g8_test_artifacts(fixture,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, artifacts.edge_audit, artifacts.schedule,
        fixture.emitter_locations, artifacts.emitter_local_home_offsets, fixture.flow_frame,
        artifacts.operation_anchors, {}, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U), {}, {});
    require(!proof.succeeded && !proof.diagnostics.empty() &&
            proof.diagnostics.front().code == "N113H004D",
        "G8 accepted a schedule whose exact branch anchor was absent from final FunctionEmitter output");
}

void test_g8_rejects_duplicate_operation_anchor_identity() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto artifacts = make_g8_test_artifacts(fixture,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    artifacts.operation_anchors.push_back(artifacts.operation_anchors.front());
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, artifacts.edge_audit, artifacts.schedule,
        fixture.emitter_locations, artifacts.emitter_local_home_offsets, fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U), {}, {});
    require(!proof.succeeded && !proof.diagnostics.empty() &&
            proof.diagnostics.front().code == "N113H0045",
        "G8 accepted a duplicate Flow operation operand anchor");
}

void test_g8_rejects_emitted_operand_location_not_owned_by_n111() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto artifacts = make_g8_test_artifacts(fixture,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    auto anchor = std::find_if(artifacts.operation_anchors.begin(), artifacts.operation_anchors.end(),
        [](const auto& item) { return item.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None; });
    require(anchor != artifacts.operation_anchors.end(), "G8 test fixture has no operand anchor to corrupt");
    anchor->emitted_operand_location = make_location(NativeRegisterLocationKind::Register, 11U);
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, artifacts.edge_audit, artifacts.schedule,
        fixture.emitter_locations, artifacts.emitter_local_home_offsets, fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U), {}, {});
    require(!proof.succeeded && !proof.diagnostics.empty() &&
            proof.diagnostics.front().code == "N113H0047",
        "G8 accepted an emitted operand anchor whose register was not the N111 active location");
}

void test_g8_rejects_missing_concrete_value_operand_anchor_for_edge_owner() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto artifacts = make_g8_test_artifacts(fixture,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    artifacts.operation_anchors.erase(std::remove_if(artifacts.operation_anchors.begin(),
        artifacts.operation_anchors.end(), [](const auto& item) {
            return item.emitted_operand_value_id == 7U &&
                item.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None;
        }), artifacts.operation_anchors.end());
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, artifacts.edge_audit, artifacts.schedule,
        fixture.emitter_locations, artifacts.emitter_local_home_offsets, fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U), {}, {});
    require(proof.succeeded && !proof.machine_operand_ownership_proven &&
            proof.operand_anchors_validated == 0U && !proof.machine_insertion_ready,
        "G8 treated a Value with no concrete emitted operand anchor as machine-owned");
}

void test_g8_checks_physical_frame_offsets_not_equal_slot_numbers() {
    auto fixture = make_g4_flow_edge_fixture_with_live_scratch_occupant();
    fixture.emitter_locations.values.at(7U).active_location =
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 0U);
    const auto audit = audit_g4_fixture(fixture);
    require(audit.succeeded, "G8 frame namespace fixture failed its G4 audit");
    const auto branches = make_g6_branch_anchors(audit);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto candidates = plan_native_x64_flow_edge_transfer_candidates(audit, branches, classes);
    require(candidates.succeeded, "G8 frame namespace fixture failed G6 candidate planning");
    const auto schedule = schedule_native_x64_flow_edge_transfer_candidates(candidates, classes,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(schedule.succeeded, "G8 frame namespace fixture failed G7 schedule planning");
    auto anchors = make_g8_operand_anchors(fixture);
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, audit, schedule, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, anchors, branches,
        make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U), {}, {});
    require(proof.succeeded && proof.endpoint_ownership_mismatches > 0U &&
            !proof.machine_operand_ownership_proven,
        "G8 equated identical numeric HomeSlot IDs despite different physical RBP offsets");
}

void test_g8_rejects_expression_reserved_memory_expansion_scratch() {
    auto fixture = make_g4_flow_edge_fixture_with_live_scratch_occupant();
    const auto audit = audit_g4_fixture(fixture);
    const auto branches = make_g6_branch_anchors(audit);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto candidates = plan_native_x64_flow_edge_transfer_candidates(audit, branches, classes);
    const auto gp = make_location(NativeRegisterLocationKind::Temporary, 11U);
    const auto xmm = make_location(NativeRegisterLocationKind::Temporary, 5U);
    const auto schedule = schedule_native_x64_flow_edge_transfer_candidates(candidates, classes, gp, xmm);
    auto anchors = make_g8_operand_anchors(fixture);
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, audit, schedule, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, anchors, branches, gp, xmm, {11U}, {});
    require(proof.succeeded && !proof.scratch_liveness_proven &&
            proof.scratch_liveness_conflicts > 0U && !proof.machine_insertion_ready,
        "G8 accepted a GP scratch register reserved by an expression temporary");
}

void test_g8_rejects_scratch_occupied_by_coalesced_live_through_value() {
    auto fixture = make_g4_flow_edge_fixture_with_live_scratch_occupant();
    const auto audit = audit_g4_fixture(fixture);
    require(audit.succeeded, "G8 live-scratch fixture did not produce a valid G4 audit");
    const auto branches = make_g6_branch_anchors(audit);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto candidates = plan_native_x64_flow_edge_transfer_candidates(audit, branches, classes);
    require(candidates.succeeded && candidates.candidate_copies == 2U,
        "G8 live-scratch fixture did not isolate the changed Value from its coalesced live occupant");
    const auto gp = make_location(NativeRegisterLocationKind::Temporary, 10U);
    const auto xmm = make_location(NativeRegisterLocationKind::Temporary, 5U);
    const auto schedule = schedule_native_x64_flow_edge_transfer_candidates(candidates, classes, gp, xmm);
    require(schedule.succeeded, "G7 should allow R10 because the changed memory endpoints do not alias it");
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, audit, schedule, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, make_g8_operand_anchors(fixture),
        branches, gp, xmm, {}, {});
    require(proof.succeeded && !proof.scratch_liveness_proven &&
            proof.scratch_liveness_conflicts > 0U,
        "G8 failed to detect a coalesced Value that remains live in the selected scratch register on the edge");
}

void test_g8_function_emitter_audit_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g8_edge_readiness(source, false);
    const auto audited = compile_with_n113_g8_edge_readiness(source, true);
    require(baseline.stats.native_n113_flow_edge_schedule_plans_built == 0U &&
            baseline.stats.native_n113_flow_edge_schedules_audited == 0U,
        "N113-G8 must remain disabled by default");
    require(audited.stats.native_n113_flow_edge_schedule_plans_built > 0U &&
            audited.stats.native_n113_flow_edge_audit_plans_built > 0U &&
            audited.stats.native_n113_flow_edge_scratch_schedule_unavailable > 0U &&
            audited.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "N113-G8 did not report the real FunctionEmitter global-scratch blocker or incorrectly claimed insertion readiness");
    require(audited.object_bytes == baseline.object_bytes &&
            audited.stats.native_n113_emitter_actions_inserted == 0U &&
            audited.stats.native_n113_flow_frame_spill_slots_reserved == 0U &&
            audited.stats.native_n113_flow_frame_home_slots_reserved == 0U,
        "N113-G8 read-only audit changed COFF bytes, inserted actions, or reserved the active Flow frame");
}

void test_g8_function_emitter_audit_composes_with_g2_without_changing_moves() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g8_edge_readiness(source, false, true);
    const auto g2_plus_g8 = compile_with_n113_g8_edge_readiness(source, true, true);
    require(g2_only.stats.native_n113_emitter_actions_inserted > 0U &&
            g2_plus_g8.stats.native_n113_flow_edge_schedule_plans_built > 0U &&
            g2_plus_g8.stats.native_n113_emitter_actions_inserted ==
                g2_only.stats.native_n113_emitter_actions_inserted &&
            g2_plus_g8.stats.native_n113_emitter_machine_moves_inserted ==
                g2_only.stats.native_n113_emitter_machine_moves_inserted &&
            g2_plus_g8.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_only.stats.native_n113_emitter_transfer_bytes_inserted &&
            g2_plus_g8.object_bytes == g2_only.object_bytes,
        "N113-G8 changed the independently enabled G2 insertion result or serialized object bytes");
}

NativeX64FlowEdgeLocationAuditPlan audit_g4_fixture(const G4FlowEdgeFixture& fixture) {
    return audit_native_x64_flow_edge_locations(
        fixture.function, fixture.analysis.liveness, fixture.analysis.intervals,
        fixture.flow_segments, fixture.home_phi, fixture.emitter_locations, fixture.flow_frame);
}

void test_g5_real_emitter_edge_audit_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g5_edge_audit(source, false);
    const auto audited = compile_with_n113_g5_edge_audit(source, true);
    require(baseline.stats.native_n113_flow_edge_audit_plans_built == 0U &&
            baseline.stats.native_n113_flow_edges_audited == 0U,
        "N113-G5 edge audit must remain disabled by default");
    require(audited.stats.native_n113_flow_edge_audit_plans_built > 0U &&
            audited.stats.native_n113_flow_edges_audited > 0U &&
            audited.stats.native_n113_flow_edge_live_values_audited > 0U,
        "N113-G5 did not run the G4 crosswalk against the real FunctionEmitter CFG");
    require(audited.object_bytes == baseline.object_bytes,
        "N113-G5 read-only FunctionEmitter audit changed serialized COFF bytes");
    require(audited.stats.native_n113_flow_frame_spill_slots_reserved == 0U &&
            audited.stats.native_n113_flow_frame_home_slots_reserved == 0U &&
            audited.stats.stack_frame_bytes == baseline.stats.stack_frame_bytes,
        "N113-G5 audit-only mode unexpectedly reserved Flow slots or enlarged the active frame");
}

void test_g5_real_emitter_edge_audit_composes_with_opt_in_g2_without_changing_g2_moves() {
    const auto source = n113_emitter_pressure_source();
    const auto g2 = compile_with_n113_g5_edge_audit(source, false, true);
    const auto g2_and_g5 = compile_with_n113_g5_edge_audit(source, true, true);
    require(g2.stats.native_n113_emitter_actions_inserted > 0U &&
            g2_and_g5.stats.native_n113_emitter_actions_inserted ==
                g2.stats.native_n113_emitter_actions_inserted &&
            g2_and_g5.stats.native_n113_emitter_machine_moves_inserted ==
                g2.stats.native_n113_emitter_machine_moves_inserted &&
            g2_and_g5.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2.stats.native_n113_emitter_transfer_bytes_inserted,
        "N113-G5 audit changed the independently enabled G2 insertion accounting");
    require(g2_and_g5.stats.native_n113_flow_edge_audit_plans_built > 0U &&
            g2_and_g5.stats.native_n113_flow_edges_audited > 0U,
        "N113-G5 did not audit CFG edges when composed with G2 insertion");
    require(g2_and_g5.object_bytes == g2.object_bytes,
        "N113-G5 audit changed the serialized COFF output of the same G2 insertion run");
}

void test_g5_edge_audit_uses_hypothetical_flow_frame_without_enabling_reservation() {
    const auto result = compile_with_n113_g5_edge_audit(n113_emitter_pressure_source(), true);
    require(result.stats.native_n113_flow_edge_audit_plans_built > 0U &&
            result.stats.native_n113_flow_frame_spill_slots_reserved == 0U &&
            result.stats.native_n113_flow_frame_home_slots_reserved == 0U,
        "N113-G5 must use a hypothetical Flow-frame map without enabling N113-F reservation");
}

void test_g4_audits_exact_diamond_edge_locations_and_home_phi_aliases() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto audit = audit_g4_fixture(fixture);
    require(audit.succeeded && audit.edges_audited == 3U &&
            audit.live_values_audited == 2U && audit.flow_location_changes == 2U &&
            audit.home_phi_nodes_audited == 1U && audit.home_phi_incoming_edges_audited == 2U,
        "G4 did not audit both live-through diamond edges and the coalesced memory Phi");
    require(audit.values.size() == 2U &&
            audit.values[0].emitter_active_location.kind == NativeRegisterLocationKind::Register &&
            audit.values[0].emitter_active_location.register_index == 12U,
        "G4 did not keep N111's authoritative emitter location separate from N112 edge endpoints");
}

void test_g4_coalesced_n112_edge_segments_need_no_candidate_transition() {
    auto fixture = make_g4_flow_edge_fixture();
    for (auto& segment : fixture.flow_segments.segments) {
        if (segment.segment.value_id == 7U) {
            segment.location.kind = NativeRegisterLocationKind::Register;
            segment.location.register_index = 8U;
        }
    }
    const auto audit = audit_g4_fixture(fixture);
    require(audit.succeeded && audit.flow_location_changes == 0U &&
            audit.flow_locations_coalesced == 2U,
        "G4 reported a transition when N112 uses one location across the audited CFG edges");
}

void test_g4_maps_flow_spill_endpoints_without_equating_emitter_home_slots() {
    auto fixture = make_g4_flow_edge_fixture();
    for (auto& segment : fixture.flow_segments.segments) {
        if (segment.segment.value_id == 7U && segment.segment.block_id == 0U) {
            segment.location.kind = NativeRegisterLocationKind::SpillSlot;
            segment.location.spill_slot = 4U;
        }
    }
    fixture.flow_frame.spill_slot_offsets.emplace(4U, -40);
    fixture.emitter_locations.values.at(7U).active_location =
        make_location(NativeRegisterLocationKind::HomeSlot, 4U, 4U);
    const auto audit = audit_g4_fixture(fixture);
    require(audit.succeeded && audit.values.size() == 2U &&
            audit.values[0].predecessor_uses_frame &&
            audit.values[0].predecessor_frame_displacement == -40 &&
            audit.values[0].emitter_active_location.kind == NativeRegisterLocationKind::HomeSlot,
        "G4 did not retain distinct Flow spill and Emitter HomeSlot namespaces with physical Flow displacement");
}

void test_g4_rejects_missing_flow_frame_mapping() {
    auto fixture = make_g4_flow_edge_fixture();
    for (auto& segment : fixture.flow_segments.segments) {
        if (segment.segment.value_id == 7U && segment.segment.block_id == 0U) {
            segment.location.kind = NativeRegisterLocationKind::SpillSlot;
            segment.location.spill_slot = 5U;
        }
    }
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H000A",
        "G4 accepted a Flow spill endpoint without a reserved physical frame displacement");
}

void test_g4_rejects_invalid_flow_frame_alignment() {
    auto fixture = make_g4_flow_edge_fixture();
    for (auto& segment : fixture.flow_segments.segments) {
        if (segment.segment.value_id == 7U && segment.segment.block_id == 0U) {
            segment.location.kind = NativeRegisterLocationKind::SpillSlot;
            segment.location.spill_slot = 5U;
        }
    }
    fixture.flow_frame.spill_slot_offsets.emplace(5U, -22);
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H000A",
        "G4 accepted a misaligned Flow-frame displacement");
}

void test_g4_rejects_duplicate_flow_edge_identity() {
    auto fixture = make_g4_flow_edge_fixture();
    fixture.function.edges.back().id = fixture.function.edges.front().id;
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H0013",
        "G4 accepted duplicate FlowEdge identity");
}

void test_g4_rejects_n111_location_missing_for_live_edge_value() {
    auto fixture = make_g4_flow_edge_fixture();
    fixture.emitter_locations.values.clear();
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H0004",
        "G4 accepted an incomplete authoritative N111 location table");
}

void test_g4_rejects_phi_alias_that_changes_source_home_identity() {
    auto fixture = make_g4_flow_edge_fixture();
    fixture.home_phi.home_aliases.back().source_home_value_id = 99U;
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H000D",
        "G4 accepted a home-Phi alias that does not identify a declared source value");
}

void test_g4_rejects_phi_incoming_from_non_predecessor() {
    auto fixture = make_g4_flow_edge_fixture();
    fixture.home_phi.phis.front().incoming.front().predecessor_block_id = 2U;
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H0011",
        "G4 accepted a home-Phi incoming version from a non-predecessor block");
}

void test_g4_rejects_missing_or_ambiguous_segment_at_edge_boundary() {
    auto missing_fixture = make_g4_flow_edge_fixture();
    const auto missing = std::find_if(missing_fixture.flow_segments.segments.begin(),
        missing_fixture.flow_segments.segments.end(), [](const auto& segment) {
            return segment.segment.value_id == 7U && segment.segment.block_id == 0U;
        });
    require(missing != missing_fixture.flow_segments.segments.end(),
        "G4 test fixture did not contain its predecessor segment");
    missing_fixture.flow_segments.segments.erase(missing);
    const auto missing_audit = audit_g4_fixture(missing_fixture);
    require(!missing_audit.succeeded && !missing_audit.diagnostics.empty() &&
            missing_audit.diagnostics.front().code == "N113H001B",
        "G4 accepted a missing predecessor boundary segment");

    auto ambiguous_fixture = make_g4_flow_edge_fixture();
    const auto original = *std::find_if(ambiguous_fixture.flow_segments.segments.begin(),
        ambiguous_fixture.flow_segments.segments.end(), [](const auto& segment) {
            return segment.segment.value_id == 7U && segment.segment.block_id == 0U;
        });
    ambiguous_fixture.flow_segments.segments.push_back(original);
    ambiguous_fixture.flow_segments.segments.back().segment.segment_id += 1000U;
    const auto ambiguous_audit = audit_g4_fixture(ambiguous_fixture);
    require(!ambiguous_audit.succeeded && !ambiguous_audit.diagnostics.empty() &&
            ambiguous_audit.diagnostics.front().code == "N113H001B",
        "G4 accepted overlapping predecessor boundary segments");
}

void test_g4_rejects_empty_block_with_live_through_value() {
    auto fixture = make_g4_flow_edge_fixture();
    fixture.function.register_ir.blocks[1].instructions.clear();
    fixture.analysis = build_fixture(fixture.function.register_ir);
    require(fixture.analysis.liveness.succeeded && fixture.analysis.intervals.succeeded &&
            fixture.analysis.segments.succeeded && fixture.analysis.allocation.succeeded,
        "G4 empty-block fixture did not build its allocator analysis");
    fixture.flow_segments = build_plan(fixture.analysis);
    require(fixture.flow_segments.succeeded, "G4 empty-block fixture could not build its Flow segment plan");
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H0018",
        "G4 silently accepted a live-through value without an addressable empty-block boundary");
}

void test_g4_rejects_incomplete_flow_edge_identity_coverage() {
    auto fixture = make_g4_flow_edge_fixture();
    fixture.home_phi.phis.clear();
    fixture.function.edges.erase(fixture.function.edges.begin() + 1);
    const auto audit = audit_g4_fixture(fixture);
    require(!audit.succeeded && !audit.diagnostics.empty() &&
            audit.diagnostics.front().code == "N113H001D",
        "G4 accepted a FlowEdge table that omitted a real CFG successor edge");
}

std::vector<X64FlowBranchAnchor> make_g6_branch_anchors(
    const NativeX64FlowEdgeLocationAuditPlan& audit) {
    std::vector<X64FlowBranchAnchor> anchors;
    std::set<FlowEdgeId> seen;
    for (const auto& value : audit.values) {
        if (!seen.insert(value.edge_id).second) continue;
        X64FlowBranchAnchor anchor;
        anchor.function_index = 0U;
        anchor.edge_id = value.edge_id;
        anchor.from_block_id = static_cast<FlowBlockId>(value.predecessor_block_id);
        anchor.to_block_id = static_cast<FlowBlockId>(value.successor_block_id);
        anchor.edge_kind = value.edge_id == 3U ? FlowEdgeKind::IfThenMerge : FlowEdgeKind::IfFalse;
        anchor.encoding = anchors.empty() ? X64FlowBranchEncoding::ConditionalRel32
                                           : X64FlowBranchEncoding::Fallthrough;
        anchor.conditional_taken = anchors.empty();
        anchor.instruction_offset = static_cast<std::uint32_t>(8U + anchors.size() * 8U);
        anchor.displacement_offset = anchor.encoding == X64FlowBranchEncoding::Fallthrough
            ? InvalidNativeFlowOperationIndex : anchor.instruction_offset + 1U;
        anchor.target_offset = static_cast<std::uint32_t>(64U + anchors.size() * 8U);
        anchor.function_code_size = 128U;
        anchors.push_back(anchor);
    }
    return anchors;
}

std::map<std::uint32_t, NativeRegisterClass> make_g6_value_classes() {
    return {{7U, NativeRegisterClass::GeneralPurpose}};
}

void test_g6_plans_exact_diamond_edge_candidates_without_claiming_machine_ready() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto audit = audit_g4_fixture(fixture);
    const auto anchors = make_g6_branch_anchors(audit);
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, anchors, make_g6_value_classes());
    require(plan.succeeded && !plan.ready_for_machine_insertion &&
            plan.flow_location_changes_considered == 2U &&
            plan.candidate_copies == 2U && plan.candidate_edges == 2U &&
            plan.candidates.size() == 2U,
        "G6 did not produce two validated but non-executable candidates for the exact diamond edges");
    for (const auto& candidate : plan.candidates) {
        require(candidate.copy.value_id == 7U &&
                candidate.copy.source.kind == NativeRegisterLocationKind::Register &&
                candidate.copy.destination.kind == NativeRegisterLocationKind::Register &&
                candidate.copy.source.register_index != candidate.copy.destination.register_index &&
                candidate.emitter_active_location.kind == NativeRegisterLocationKind::Register &&
                candidate.emitter_active_location.register_index == 12U &&
                candidate.branch_anchor.edge_id == candidate.edge_id &&
                candidate.branch_anchor.from_block_id == candidate.predecessor_block_id &&
                candidate.branch_anchor.to_block_id == candidate.successor_block_id,
            "G6 candidate lost exact Flow endpoints, N111 authority, or branch-edge identity");
    }
}

void test_g6_coalesced_edge_locations_produce_no_candidates() {
    auto fixture = make_g4_flow_edge_fixture();
    for (auto& segment : fixture.flow_segments.segments) {
        if (segment.segment.value_id == 7U) {
            segment.location.kind = NativeRegisterLocationKind::Register;
            segment.location.register_index = 8U;
        }
    }
    const auto audit = audit_g4_fixture(fixture);
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, {}, make_g6_value_classes());
    require(audit.succeeded && plan.succeeded && plan.candidates.empty() &&
            plan.candidate_copies == 0U && plan.candidate_edges == 0U &&
            !plan.ready_for_machine_insertion,
        "G6 emitted a candidate when N112 locations were already coalesced");
}

void test_g6_rejects_missing_exact_branch_anchor() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto audit = audit_g4_fixture(fixture);
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, {}, make_g6_value_classes());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0028",
        "G6 accepted a changed Flow location without an exact machine branch/fallthrough anchor");
}

void test_g6_rejects_duplicate_branch_anchor_identity() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto audit = audit_g4_fixture(fixture);
    auto anchors = make_g6_branch_anchors(audit);
    anchors.push_back(anchors.front());
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, anchors, make_g6_value_classes());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0021",
        "G6 accepted duplicate branch anchors for one CFG Edge ID");
}

void test_g6_rejects_branch_anchor_with_mismatched_edge_endpoints() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto audit = audit_g4_fixture(fixture);
    auto anchors = make_g6_branch_anchors(audit);
    anchors.front().to_block_id = static_cast<FlowBlockId>(99U);
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, anchors, make_g6_value_classes());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H002A",
        "G6 accepted a branch anchor whose endpoints do not match its FlowEdge identity");
}

void test_g6_rejects_register_class_mismatch() {
    const auto fixture = make_g4_flow_edge_fixture();
    const auto audit = audit_g4_fixture(fixture);
    auto classes = make_g6_value_classes();
    classes[7U] = NativeRegisterClass::FloatingPoint;
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, make_g6_branch_anchors(audit), classes);
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0023",
        "G6 accepted an edge candidate whose N112 value class disagrees with the value table");
}

void test_g6_rejects_temporary_flow_endpoint() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto audit = audit_g4_fixture(fixture);
    audit.values.front().predecessor_flow_location.kind = NativeRegisterLocationKind::Temporary;
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, make_g6_branch_anchors(audit), make_g6_value_classes());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0025",
        "G6 accepted a temporary register as a persistent CFG edge endpoint");
}

void test_g6_rejects_invalid_flow_frame_endpoint_mapping() {
    const auto fixture = make_g4_flow_edge_fixture();
    auto audit = audit_g4_fixture(fixture);
    auto& value = audit.values.front();
    value.predecessor_flow_location.kind = NativeRegisterLocationKind::SpillSlot;
    value.predecessor_flow_location.spill_slot = 4U;
    value.predecessor_uses_frame = true;
    value.predecessor_frame_displacement = -22;
    const auto plan = plan_native_x64_flow_edge_transfer_candidates(
        audit, make_g6_branch_anchors(audit), make_g6_value_classes());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0025",
        "G6 accepted a misaligned Flow spill displacement in an edge candidate");
}

X64FlowBranchAnchor make_g7_test_anchor(FlowEdgeId edge_id,
                                              NativeRegisterBlockId predecessor,
                                              NativeRegisterBlockId successor,
                                              std::uint32_t instruction_offset = 2U) {
    X64FlowBranchAnchor anchor;
    anchor.function_index = 0U;
    anchor.edge_id = edge_id;
    anchor.from_block_id = predecessor;
    anchor.to_block_id = successor;
    anchor.encoding = X64FlowBranchEncoding::Fallthrough;
    anchor.displacement_offset = InvalidNativeFlowOperationIndex;
    anchor.instruction_offset = instruction_offset;
    anchor.target_offset = 12U;
    anchor.function_code_size = 16U;
    return anchor;
}

NativeX64FlowEdgeTransferCandidate make_g7_candidate(
    FlowEdgeId edge_id, NativeRegisterBlockId predecessor,
    NativeRegisterBlockId successor, std::uint32_t value_id,
    NativeRegisterLocation source, NativeRegisterLocation destination,
    X64FlowBranchAnchor anchor) {
    NativeX64FlowEdgeTransferCandidate candidate;
    candidate.edge_id = edge_id;
    candidate.predecessor_block_id = predecessor;
    candidate.successor_block_id = successor;
    candidate.branch_anchor = anchor;
    candidate.copy = NativeRegisterParallelCopy{value_id, source, destination};
    candidate.emitter_active_location = make_location(NativeRegisterLocationKind::Register, 12U);
    return candidate;
}

NativeX64FlowEdgeTransferCandidatePlan make_g7_candidate_plan(
    std::vector<NativeX64FlowEdgeTransferCandidate> candidates,
    std::uint32_t edge_count) {
    NativeX64FlowEdgeTransferCandidatePlan plan;
    plan.succeeded = true;
    plan.ready_for_machine_insertion = false;
    plan.candidate_copies = static_cast<std::uint32_t>(candidates.size());
    plan.candidate_edges = edge_count;
    plan.candidates = std::move(candidates);
    return plan;
}

NativeX64FlowEdgeTransferCandidatePlan make_g11_candidate_plan(
    const std::vector<std::uint32_t>& value_ids = {7U}) {
    std::vector<NativeX64FlowEdgeTransferCandidate> candidates;
    const auto anchor = make_g7_test_anchor(71U, 1U, 2U);
    for (const auto value_id : value_ids) {
        candidates.push_back(make_g7_candidate(71U, 1U, 2U, value_id,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor));
    }
    return make_g7_candidate_plan(std::move(candidates), 1U);
}

NativeX64FlowEdgePerEdgeScratchPlan make_g11_scratch_plan(
    const std::vector<std::uint32_t>& value_ids = {7U},
    bool endpoint_owned = true, bool scratch_proven = true,
    bool include_schedule_values = true) {
    NativeX64FlowEdgePerEdgeScratchPlan plan;
    plan.succeeded = true;
    plan.candidate_edges = 1U;
    plan.scheduled_edges = 1U;
    plan.scratch_proven_edges = scratch_proven ? 1U : 0U;
    plan.scratch_unavailable_edges = scratch_proven ? 0U : 1U;
    NativeX64FlowEdgePerEdgeScratchEntry edge;
    edge.edge_id = 71U;
    edge.schedule_built = true;
    edge.scratch_selection_proven = scratch_proven;
    edge.schedule.edge_id = 71U;
    edge.schedule.predecessor_block_id = 1U;
    edge.schedule.successor_block_id = 2U;
    edge.schedule.branch_anchor = make_g7_test_anchor(71U, 1U, 2U);
    edge.schedule.bundle.edge_id = 71U;
    if (include_schedule_values) {
        for (const auto value_id : value_ids) {
            edge.schedule.bundle.operations.push_back(NativeRegisterParallelCopyOperation{
                value_id,
                make_location(NativeRegisterLocationKind::Register, 8U),
                make_location(NativeRegisterLocationKind::Register, 9U), false});
        }
    }
    edge.readiness.edge_id = 71U;
    edge.readiness.live_values_audited = static_cast<std::uint32_t>(value_ids.size());
    edge.readiness.endpoint_ownership_matches = endpoint_owned
        ? static_cast<std::uint32_t>(value_ids.size() * 2U) : 0U;
    edge.readiness.endpoint_ownership_mismatches = endpoint_owned ? 0U : 1U;
    edge.readiness.operand_anchors_validated = endpoint_owned ? 1U : 0U;
    edge.readiness.machine_operand_ownership_proven = endpoint_owned;
    edge.readiness.scratch_liveness_proven = scratch_proven;
    plan.edges.push_back(std::move(edge));
    return plan;
}

NativeX64FlowOperationOperandOwnershipPlan make_g11_operand_plan(
    const std::vector<std::uint32_t>& value_ids = {7U},
    const std::vector<std::uint32_t>& mismatched_values = {}) {
    NativeX64FlowOperationOperandOwnershipPlan plan;
    plan.succeeded = true;
    plan.function_index = 0U;
    for (std::size_t i = 0; i < value_ids.size(); ++i) {
        NativeX64FlowOperationOperandOwnershipEntry operand;
        operand.flow_operation_id = 100U + i;
        operand.flow_block_id = 1U;
        operand.flow_operation_index = static_cast<std::uint32_t>(i);
        operand.value_id = value_ids[i];
        operand.register_class = NativeRegisterClass::GeneralPurpose;
        operand.operand_kind = X64FlowEmitterAnchor::OperandKind::ValueRead;
        operand.flow_operation_location = make_location(NativeRegisterLocationKind::Register, 12U);
        operand.emitter_operand_location = make_location(NativeRegisterLocationKind::Register, 12U);
        operand.location_matches = std::find(mismatched_values.begin(), mismatched_values.end(), value_ids[i]) ==
            mismatched_values.end();
        if (!operand.location_matches)
            operand.emitter_operand_location = make_location(NativeRegisterLocationKind::Register, 13U);
        plan.operands.push_back(operand);
        ++plan.operand_operations_audited;
        if (operand.location_matches) ++plan.operand_location_matches;
        else ++plan.operand_location_mismatches;
    }
    plan.source_operations_audited = plan.operand_operations_audited;
    plan.all_operands_owned = plan.operand_location_mismatches == 0U;
    return plan;
}

std::map<std::uint32_t, NativeRegisterClass> make_g11_value_classes(
    const std::vector<std::uint32_t>& value_ids = {7U}) {
    std::map<std::uint32_t, NativeRegisterClass> classes;
    for (const auto value_id : value_ids)
        classes.emplace(value_id, NativeRegisterClass::GeneralPurpose);
    return classes;
}

void test_g11_passes_only_when_g8_g9_and_g10_proofs_intersect() {
    const auto candidates = make_g11_candidate_plan();
    const auto scratch = make_g11_scratch_plan();
    const auto operands = make_g11_operand_plan();
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        candidates, scratch, operands, make_g11_value_classes(), 0U);
    require(plan.succeeded && plan.candidate_edges == 1U &&
            plan.proof_gate_passed_edges == 1U && plan.blocked_edges == 0U &&
            plan.edges.size() == 1U && plan.edges.front().proof_gate_passed &&
            plan.edges.front().exact_g9_schedule &&
            plan.edges.front().endpoint_ownership_proven &&
            plan.edges.front().scratch_liveness_proven &&
            plan.edges.front().operation_operand_ownership_proven,
        "G11 did not require and intersect exact schedule, endpoint, scratch and operation-operand proofs");
}

void test_g11_blocks_edge_when_g9_scratch_is_not_proven() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan({7U}, true, false),
        make_g11_operand_plan(), make_g11_value_classes(), 0U);
    require(plan.succeeded && plan.blocked_edges == 1U &&
            plan.scratch_blocked_edges == 1U && plan.proof_gate_passed_edges == 0U &&
            !plan.edges.front().proof_gate_passed,
        "G11 accepted an edge without a proven edge-local GP/XMM scratch selection");
}

void test_g11_blocks_edge_when_g8_endpoint_ownership_is_incomplete() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan({7U}, false, true),
        make_g11_operand_plan(), make_g11_value_classes(), 0U);
    require(plan.succeeded && plan.blocked_edges == 1U &&
            plan.endpoint_ownership_blocked_edges == 1U && plan.proof_gate_passed_edges == 0U,
        "G11 accepted an edge whose G8 N112-to-emitter endpoint ownership was incomplete");
}

void test_g11_blocks_edge_when_candidate_value_has_no_g10_operand_evidence() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan({7U, 8U}), make_g11_scratch_plan({7U, 8U}),
        make_g11_operand_plan({7U}), make_g11_value_classes({7U, 8U}), 0U);
    require(plan.succeeded && plan.blocked_edges == 1U &&
            plan.operation_operand_blocked_edges == 1U &&
            plan.edges.front().operation_values_proven == 1U &&
            !plan.edges.front().operation_operand_ownership_proven,
        "G11 accepted an edge candidate whose ValueId has no concrete G10 operation-operand evidence");
}

void test_g11_blocks_edge_when_candidate_value_has_any_g10_mismatch() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan(),
        make_g11_operand_plan({7U, 7U}, {7U}), make_g11_value_classes(), 0U);
    require(plan.succeeded && plan.blocked_edges == 1U &&
            plan.operation_operand_blocked_edges == 1U &&
            !plan.edges.front().operation_operand_ownership_proven,
        "G11 ignored a mismatched G10 operand for a ValueId used by this edge");
}

void test_g11_ignores_unrelated_g10_mismatch_on_another_value() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan({7U}), make_g11_scratch_plan({7U}),
        make_g11_operand_plan({7U, 8U}, {8U}), make_g11_value_classes({7U, 8U}), 0U);
    require(plan.succeeded && plan.proof_gate_passed_edges == 1U &&
            plan.operation_operand_blocked_edges == 0U &&
            plan.edges.front().operation_operand_ownership_proven,
        "G11 globally blocked an edge for an unrelated G10 mismatch owned by a different ValueId");
}

void test_g11_rejects_cross_function_g10_crosswalk() {
    auto operands = make_g11_operand_plan();
    operands.function_index = 1U;
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan(), operands,
        make_g11_value_classes(), 0U);
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113K0001",
        "G11 accepted G10 operand evidence from a different FunctionEmitter function index");
}

void test_g11_rejects_g9_schedule_with_wrong_edge_identity() {
    auto scratch = make_g11_scratch_plan();
    scratch.edges.front().schedule.branch_anchor.edge_id = 72U;
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), scratch, make_g11_operand_plan(),
        make_g11_value_classes(), 0U);
    require(plan.succeeded && plan.blocked_edges == 1U &&
            plan.schedule_blocked_edges == 1U && !plan.edges.front().proof_gate_passed,
        "G11 accepted a G9 schedule detached from its exact FlowEdgeId branch anchor");
}

void test_g11_rejects_schedule_missing_one_candidate_value() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan({7U, 8U}),
        make_g11_scratch_plan({7U, 8U}, true, true, false),
        make_g11_operand_plan({7U, 8U}), make_g11_value_classes({7U, 8U}), 0U);
    require(plan.succeeded && plan.blocked_edges == 1U &&
            plan.schedule_blocked_edges == 1U && !plan.edges.front().proof_gate_passed,
        "G11 accepted a scheduled bundle that omitted a G6 candidate ValueId");
}

void test_g11_function_emitter_audit_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto audited = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.object_bytes == audited.object_bytes &&
            baseline.stats.native_n113_flow_edge_bundle_eligibility_plans == 0U &&
            audited.stats.native_n113_flow_edge_bundle_eligibility_plans > 0U &&
            audited.stats.native_n113_flow_edge_per_edge_plans_built > 0U &&
            audited.stats.native_n113_flow_operation_operand_ownership_plans > 0U,
        "G11 changed emitted object bytes or failed to run its G9/G10 prerequisite audits when opted in");
}

void test_g11_eligibility_composes_with_g2_without_changing_moves_or_bytes() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g11_edge_eligibility(source, false, true);
    const auto g2_plus_g11 = compile_with_n113_g11_edge_eligibility(source, true, true);
    require(g2_only.stats.native_n113_emitter_actions_inserted > 0U &&
            g2_plus_g11.stats.native_n113_flow_edge_bundle_eligibility_plans > 0U &&
            g2_plus_g11.stats.native_n113_emitter_actions_inserted ==
                g2_only.stats.native_n113_emitter_actions_inserted &&
            g2_plus_g11.stats.native_n113_emitter_machine_moves_inserted ==
                g2_only.stats.native_n113_emitter_machine_moves_inserted &&
            g2_plus_g11.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_only.stats.native_n113_emitter_transfer_bytes_inserted &&
            g2_plus_g11.object_bytes == g2_only.object_bytes,
        "G11 changed independently enabled G2 spill/reload moves, inserted bytes or emitted object bytes");
}

void test_g12_emits_one_function_report_per_function_when_enabled() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.succeeded() && result.stats.native_n113_flow_edge_bundle_function_reports.size() == 4U &&
            result.stats.native_n113_flow_edge_bundle_eligibility_plans == 4U,
        "G12 did not publish one per-function report for each emitted FunctionEmitter");
    for (std::size_t i = 0U; i < result.stats.native_n113_flow_edge_bundle_function_reports.size(); ++i) {
        const auto& report = result.stats.native_n113_flow_edge_bundle_function_reports[i];
        require(report.function_index == i && !report.function_name.empty(),
            "G12 function report lost source function identity or original CompilationUnit ordering");
    }
}

void test_g12_per_function_counters_sum_to_g11_aggregate_stats() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    std::uint32_t candidates = 0U, passed = 0U, blocked = 0U;
    std::uint32_t schedule = 0U, endpoint = 0U, scratch = 0U, operand = 0U;
    for (const auto& report : result.stats.native_n113_flow_edge_bundle_function_reports) {
        candidates += report.candidate_edges;
        passed += report.proof_gate_passed_edges;
        blocked += report.blocked_edges;
        schedule += report.schedule_blocked_edges;
        endpoint += report.endpoint_ownership_blocked_edges;
        scratch += report.scratch_blocked_edges;
        operand += report.operation_operand_blocked_edges;
    }
    require(candidates == result.stats.native_n113_flow_edge_bundle_candidate_edges &&
            passed == result.stats.native_n113_flow_edge_bundle_proof_gate_passed &&
            blocked == result.stats.native_n113_flow_edge_bundle_blocked &&
            schedule == result.stats.native_n113_flow_edge_bundle_schedule_blocked &&
            endpoint == result.stats.native_n113_flow_edge_bundle_endpoint_blocked &&
            scratch == result.stats.native_n113_flow_edge_bundle_scratch_blocked &&
            operand == result.stats.native_n113_flow_edge_bundle_operand_blocked,
        "G12 per-function counts do not reconcile to existing G11 aggregate counters");
}

void test_g12_edge_reports_retain_exact_ids_and_blocker_conjunction() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    std::size_t edge_count = 0U;
    for (const auto& report : result.stats.native_n113_flow_edge_bundle_function_reports) {
        require(report.plan_succeeded,
            "G12 marked a FunctionEmitter's G11 plan failed for the real pressure fixture");
        require(report.edges.size() == report.candidate_edges,
            "G12 per-edge report count does not match the owning function's candidate edge count");
        edge_count += report.edges.size();
        for (const auto& edge : report.edges) {
            require(edge.edge_id != 0U && edge.candidate_values == edge.value_ids.size(),
                "G12 lost exact edge identity or candidate ValueIds");
            const bool conjunction = edge.exact_g9_schedule && edge.endpoint_ownership_proven &&
                edge.scratch_liveness_proven && edge.operation_operand_ownership_proven;
            require(edge.proof_gate_passed == conjunction,
                "G12 report proof_gate_passed does not equal the G11 evidence conjunction");
            require(edge.proof_gate_passed == edge.blockers.empty(),
                "G12 per-edge blocker details disagree with the proof gate result");
        }
    }
    require(edge_count == result.stats.native_n113_flow_edge_bundle_candidate_edges,
        "G12 flattened edge records do not match the aggregate candidate inventory");
}

void test_g12_no_per_function_reports_when_g11_disabled() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), false);
    require(result.succeeded() && result.stats.native_n113_flow_edge_bundle_function_reports.empty() &&
            result.stats.native_n113_flow_edge_bundle_eligibility_plans == 0U,
        "G12 emitted census reports when the opt-in G11 audit was disabled");
}

void test_g12_function_and_edge_report_order_is_deterministic() {
    const auto source = n113_emitter_pressure_source();
    const auto first = compile_with_n113_g11_edge_eligibility(source, true);
    const auto second = compile_with_n113_g11_edge_eligibility(source, true);
    require(first.stats.native_n113_flow_edge_bundle_function_reports.size() ==
                second.stats.native_n113_flow_edge_bundle_function_reports.size(),
        "G12 report count changed across identical compilations");
    for (std::size_t i = 0U; i < first.stats.native_n113_flow_edge_bundle_function_reports.size(); ++i) {
        const auto& a = first.stats.native_n113_flow_edge_bundle_function_reports[i];
        const auto& b = second.stats.native_n113_flow_edge_bundle_function_reports[i];
        require(a.function_index == b.function_index && a.function_name == b.function_name &&
                a.plan_succeeded == b.plan_succeeded && a.candidate_edges == b.candidate_edges &&
                a.proof_gate_passed_edges == b.proof_gate_passed_edges &&
                a.blocked_edges == b.blocked_edges && a.edges.size() == b.edges.size(),
            "G12 function report summary is nondeterministic");
        for (std::size_t j = 0U; j < a.edges.size(); ++j) {
            require(a.edges[j].edge_id == b.edges[j].edge_id &&
                    a.edges[j].predecessor_block_id == b.edges[j].predecessor_block_id &&
                    a.edges[j].successor_block_id == b.edges[j].successor_block_id &&
                    a.edges[j].value_ids == b.edges[j].value_ids &&
                    a.edges[j].blockers == b.edges[j].blockers &&
                    a.edges[j].proof_gate_passed == b.edges[j].proof_gate_passed,
                "G12 exact edge report ordering/details are nondeterministic");
        }
    }
}

void test_g12_per_function_census_is_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto audited = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.object_bytes == audited.object_bytes &&
            audited.stats.native_n113_flow_edge_bundle_function_reports.size() == 4U,
        "G12 read-only census changed COFF bytes or failed to publish function reports");
}

void test_g12_census_composes_with_g2_without_changing_inserted_moves() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g11_edge_eligibility(source, false, true);
    const auto g2_g11 = compile_with_n113_g11_edge_eligibility(source, true, true);
    require(g2_only.stats.native_n113_emitter_actions_inserted > 0U &&
            g2_g11.stats.native_n113_flow_edge_bundle_function_reports.size() == 4U &&
            g2_only.stats.native_n113_emitter_actions_inserted ==
                g2_g11.stats.native_n113_emitter_actions_inserted &&
            g2_only.stats.native_n113_emitter_machine_moves_inserted ==
                g2_g11.stats.native_n113_emitter_machine_moves_inserted &&
            g2_only.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_g11.stats.native_n113_emitter_transfer_bytes_inserted &&
            g2_only.object_bytes == g2_g11.object_bytes,
        "G12 census changed independently enabled G2 machine moves or object bytes");
}

void test_g12_reports_are_isolated_between_compile_results() {
    const auto first = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    const auto second = compile_with_n113_g11_edge_eligibility(
        "module N113G12Small; public function Main() returns int { return 9; }", true);
    require(first.stats.native_n113_flow_edge_bundle_function_reports.size() == 4U &&
            second.stats.native_n113_flow_edge_bundle_function_reports.size() == 1U &&
            first.stats.native_n113_flow_edge_bundle_function_reports.front().function_index == 0U &&
            second.stats.native_n113_flow_edge_bundle_function_reports.front().function_index == 0U,
        "G12 report storage leaked across independent compile results");
}

void test_g12_reports_real_emitter_candidate_inventory_and_blockers() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.stats.native_n113_flow_edge_bundle_function_reports.size() == 4U,
        "G12 real-emitter fixture did not report every body function");
    const auto main_report = std::find_if(
        result.stats.native_n113_flow_edge_bundle_function_reports.begin(),
        result.stats.native_n113_flow_edge_bundle_function_reports.end(),
        [](const auto& report) { return report.function_name == "Main"; });
    require(main_report != result.stats.native_n113_flow_edge_bundle_function_reports.end() &&
            main_report->candidate_edges == 6U && main_report->proof_gate_passed_edges == 0U &&
            main_report->blocked_edges == 6U && main_report->edges.size() == 6U &&
            main_report->schedule_blocked_edges == 5U &&
            main_report->endpoint_ownership_blocked_edges == 6U &&
            main_report->scratch_blocked_edges == 5U &&
            main_report->operation_operand_blocked_edges == 6U,
        "G12 did not expose the real FunctionEmitter census: Main should report six blocked candidate edges and their exact blocker breakdown");
    const auto first_edge = std::find_if(main_report->edges.begin(), main_report->edges.end(),
        [](const auto& edge) { return edge.edge_id == 3U; });
    require(first_edge != main_report->edges.end() && first_edge->exact_g9_schedule &&
            first_edge->scratch_liveness_proven && !first_edge->endpoint_ownership_proven &&
            !first_edge->operation_operand_ownership_proven && !first_edge->proof_gate_passed,
        "G12 lost the exact blocker evidence for the first real FunctionEmitter candidate edge");
}

void test_g12_each_report_has_consistent_blocked_reason_counters() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    for (const auto& report : result.stats.native_n113_flow_edge_bundle_function_reports) {
        require(report.proof_gate_passed_edges + report.blocked_edges == report.candidate_edges &&
                report.schedule_blocked_edges <= report.blocked_edges &&
                report.endpoint_ownership_blocked_edges <= report.blocked_edges &&
                report.scratch_blocked_edges <= report.blocked_edges &&
                report.operation_operand_blocked_edges <= report.blocked_edges,
            "G12 per-function blocker counters are internally inconsistent");
    }
}

void test_g12_report_diagnostics_preserve_g11_failure_evidence() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    for (const auto& report : result.stats.native_n113_flow_edge_bundle_function_reports) {
        for (const auto& diagnostic : report.diagnostics) {
            require(!diagnostic.code.empty() && !diagnostic.message.empty(),
                "G12 dropped the code or message of a G11 plan diagnostic");
        }
    }
}

void test_g12_census_does_not_claim_machine_insertion_readiness() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.succeeded() && result.stats.native_n113_flow_edge_bundle_candidate_edges ==
                result.stats.native_n113_flow_edge_bundle_proof_gate_passed +
                result.stats.native_n113_flow_edge_bundle_blocked &&
            result.stats.native_n113_flow_edge_bundle_function_reports.size() == 4U,
        "G12 census failed to retain evidence-only accounting");
    // No insertion API is called by G12; G2 insertion counters remain zero here.
    require(result.stats.native_n113_emitter_actions_inserted == 0U &&
            result.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            result.stats.native_n113_emitter_transfer_bytes_inserted == 0U,
        "G12 eligibility census unexpectedly inserted FunctionEmitter machine actions");
}

void test_g12_edge_reports_preserve_value_coverage_details() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    for (const auto& report : result.stats.native_n113_flow_edge_bundle_function_reports) {
        for (const auto& edge : report.edges) {
            require(edge.operation_values_proven <= edge.candidate_values &&
                    edge.value_ids.size() == edge.candidate_values,
                "G12 per-edge report changed the number of candidate or proven ValueIds");
        }
    }
}

void test_g13_reports_each_candidate_value_with_independent_operand_evidence() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan({7U, 8U}), make_g11_scratch_plan({7U, 8U}),
        make_g11_operand_plan({7U, 8U}), make_g11_value_classes({7U, 8U}), 0U);
    require(plan.succeeded && plan.edges.size() == 1U &&
            plan.edges.front().value_operand_evidence.size() == 2U &&
            plan.edges.front().operation_values_proven == 2U &&
            plan.edges.front().operation_values_with_evidence == 2U &&
            plan.edges.front().operation_values_mismatched == 0U &&
            plan.edges.front().operation_values_missing == 0U,
        "G13 did not publish one independently proven evidence record for each candidate ValueId");
    const auto& first = plan.edges.front().value_operand_evidence.front();
    require(first.value_id == 7U && first.concrete_evidence_found &&
            first.operand_operations == 1U && first.matching_operations == 1U &&
            first.mismatching_operations == 0U && first.all_observed_operands_match,
        "G13 candidate ValueId evidence summary is incomplete or inconsistent");
}

void test_g13_reports_missing_g10_evidence_per_value() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan({7U, 8U}), make_g11_scratch_plan({7U, 8U}),
        make_g11_operand_plan({7U}), make_g11_value_classes({7U, 8U}), 0U);
    require(plan.succeeded && plan.edges.front().operation_values_with_evidence == 1U &&
            plan.edges.front().operation_values_proven == 1U &&
            plan.edges.front().operation_values_missing == 1U &&
            plan.edges.front().operation_values_mismatched == 0U,
        "G13 did not distinguish a candidate ValueId with no concrete G10 evidence");
    const auto missing = std::find_if(plan.edges.front().value_operand_evidence.begin(),
        plan.edges.front().value_operand_evidence.end(),
        [](const auto& item) { return item.value_id == 8U; });
    require(missing != plan.edges.front().value_operand_evidence.end() &&
            !missing->concrete_evidence_found && missing->operand_operations == 0U &&
            !missing->all_observed_operands_match && missing->operations.empty(),
        "G13 missing-evidence ValueId was not represented explicitly");
}

void test_g13_retains_all_operation_ids_for_a_candidate_value() {
    auto operands = make_g11_operand_plan({7U, 7U, 8U});
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan({7U, 8U}), make_g11_scratch_plan({7U, 8U}),
        operands, make_g11_value_classes({7U, 8U}), 0U);
    const auto value = std::find_if(plan.edges.front().value_operand_evidence.begin(),
        plan.edges.front().value_operand_evidence.end(),
        [](const auto& item) { return item.value_id == 7U; });
    require(value != plan.edges.front().value_operand_evidence.end() &&
            value->operand_operations == 2U && value->matching_operations == 2U &&
            value->mismatching_operations == 0U && value->operations.size() == 2U &&
            value->operations[0].flow_operation_id == 100U &&
            value->operations[1].flow_operation_id == 101U,
        "G13 did not retain every matching concrete FlowOperationId for a candidate ValueId");
}

void test_g13_preserves_exact_mismatch_operation_and_emitter_location() {
    auto operands = make_g11_operand_plan({7U, 7U});
    operands.operands[1].location_matches = false;
    operands.operands[1].emitter_operand_location = make_location(NativeRegisterLocationKind::Register, 13U);
    ++operands.operand_location_mismatches;
    --operands.operand_location_matches;
    operands.all_operands_owned = false;
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan(),
        operands, make_g11_value_classes(), 0U);
    const auto& value = plan.edges.front().value_operand_evidence.front();
    require(value.value_id == 7U && value.operand_operations == 2U &&
            value.matching_operations == 1U && value.mismatching_operations == 1U &&
            !value.all_observed_operands_match && value.operations.size() == 2U &&
            value.operations[1].flow_operation_id == 101U &&
            !value.operations[1].location_matches &&
            value.operations[1].flow_operation_location.register_index == 12U &&
            value.operations[1].emitter_operand_location.register_index == 13U,
        "G13 lost the exact G10 mismatch operation or its physical register evidence");
}

void test_g13_proven_value_count_requires_all_observed_operations_to_match() {
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan(),
        make_g11_operand_plan({7U, 7U}, {7U}), make_g11_value_classes(), 0U);
    require(plan.succeeded && plan.edges.front().operation_values_with_evidence == 1U &&
            plan.edges.front().operation_values_proven == 0U &&
            plan.edges.front().operation_values_mismatched == 1U &&
            plan.edges.front().operation_operand_ownership_proven == false,
        "G13 operation_values_proven counted a partially mismatched ValueId as fully proven");
}

void test_g13_preserves_physical_frame_displacement_mismatch() {
    auto operands = make_g11_operand_plan();
    auto& operand = operands.operands.front();
    operand.flow_location_uses_frame = true;
    operand.emitter_location_uses_frame = true;
    operand.flow_frame_displacement = -32;
    operand.emitter_frame_displacement = -40;
    operand.location_matches = false;
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan(),
        operands, make_g11_value_classes(), 0U);
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(!operation.location_matches && operation.flow_location_uses_frame &&
            operation.emitter_location_uses_frame && operation.flow_frame_displacement == -32 &&
            operation.emitter_frame_displacement == -40,
        "G13 did not retain the exact physical frame displacement mismatch from G10");
}

void test_g13_orders_value_and_operation_evidence_deterministically() {
    auto operands = make_g11_operand_plan({8U, 7U, 7U});
    std::reverse(operands.operands.begin(), operands.operands.end());
    const auto plan = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan({8U, 7U}), make_g11_scratch_plan({8U, 7U}),
        operands, make_g11_value_classes({7U, 8U}), 0U);
    const auto& evidence = plan.edges.front().value_operand_evidence;
    require(evidence.size() == 2U && evidence[0].value_id == 7U && evidence[1].value_id == 8U &&
            evidence[0].operations.size() == 2U &&
            evidence[0].operations[0].flow_operation_id == 101U &&
            evidence[0].operations[1].flow_operation_id == 102U,
        "G13 value or FlowOperation evidence ordering depends on input container order");
}

void test_g13_function_emitter_reports_value_level_g10_blockers() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    const auto main_report = std::find_if(
        result.stats.native_n113_flow_edge_bundle_function_reports.begin(),
        result.stats.native_n113_flow_edge_bundle_function_reports.end(),
        [](const auto& report) { return report.function_name == "Main"; });
    require(main_report != result.stats.native_n113_flow_edge_bundle_function_reports.end() &&
            !main_report->edges.empty(),
        "G13 FunctionEmitter census did not include Main edge reports");
    const auto edge = std::find_if(main_report->edges.begin(), main_report->edges.end(),
        [](const auto& item) { return item.edge_id == 3U; });
    require(edge != main_report->edges.end() &&
            edge->value_operand_evidence.size() == edge->value_ids.size(),
        "G13 real FunctionEmitter edge did not publish one Value evidence record per candidate ValueId");
    require(std::any_of(edge->value_operand_evidence.begin(), edge->value_operand_evidence.end(),
        [](const auto& value) {
            return value.concrete_evidence_found && value.mismatching_operations > 0U &&
                !value.all_observed_operands_match && !value.operations.empty();
        }),
        "G13 real FunctionEmitter edge did not expose a concrete mismatching G10 operation");
}

void test_g13_census_value_evidence_remains_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto audited = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.succeeded() && audited.succeeded() && baseline.object_bytes == audited.object_bytes &&
            audited.stats.native_n113_flow_edge_bundle_function_reports.size() == 4U,
        "G13 value-level census changed emitted COFF bytes or failed to report functions");
}

void test_g13_composes_with_g2_without_changing_machine_moves_or_bytes() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g11_edge_eligibility(source, false, true);
    const auto g2_plus_g13 = compile_with_n113_g11_edge_eligibility(source, true, true);
    require(g2_only.succeeded() && g2_plus_g13.succeeded() &&
            g2_plus_g13.stats.native_n113_emitter_actions_inserted ==
                g2_only.stats.native_n113_emitter_actions_inserted &&
            g2_plus_g13.stats.native_n113_emitter_machine_moves_inserted ==
                g2_only.stats.native_n113_emitter_machine_moves_inserted &&
            g2_plus_g13.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_only.stats.native_n113_emitter_transfer_bytes_inserted &&
            g2_plus_g13.object_bytes == g2_only.object_bytes,
        "G13 value-level census changed independent G2 insertion or COFF output");
}

NativeX64FlowEdgeBundleEligibilityPlan make_g14_single_mismatch_plan(
    NativeRegisterLocation flow_location,
    NativeRegisterLocation emitter_location,
    bool flow_uses_frame = false,
    bool emitter_uses_frame = false,
    std::int32_t flow_frame_displacement = 0,
    std::int32_t emitter_frame_displacement = 0,
    NativeRegisterClass flow_register_class = NativeRegisterClass::GeneralPurpose) {
    auto operands = make_g11_operand_plan();
    auto& operand = operands.operands.front();
    operand.register_class = flow_register_class;
    operand.flow_operation_location = flow_location;
    operand.emitter_operand_location = emitter_location;
    operand.flow_location_uses_frame = flow_uses_frame;
    operand.emitter_location_uses_frame = emitter_uses_frame;
    operand.flow_frame_displacement = flow_frame_displacement;
    operand.emitter_frame_displacement = emitter_frame_displacement;
    operand.location_matches = false;
    operands.operand_location_matches = 0U;
    operands.operand_location_mismatches = 1U;
    operands.all_operands_owned = false;
    auto result = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan(), operands,
        make_g11_value_classes(), 0U);
    if (!result.succeeded || result.edges.empty())
        throw std::runtime_error("G14/G15 mismatch fixture did not produce one auditable Edge: " +
            (result.diagnostics.empty() ? std::string("no diagnostic") : result.diagnostics.front().code +
                " " + result.diagnostics.front().message));
    return result;
}

void test_g14_classifies_register_index_mismatch() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 13U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded && operation.mismatch_kind == NativeX64FlowOperandMismatchKind::RegisterIndexMismatch &&
            plan.mismatch_counts.register_index == 1U &&
            plan.edges.front().mismatch_counts.register_index == 1U &&
            plan.edges.front().value_operand_evidence.front().mismatch_counts.register_index == 1U,
        "G14 did not classify a valid GP register-index mismatch at operation/value/edge levels");
}

void test_g14_classifies_invalid_register_mismatch() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 4U),
        make_location(NativeRegisterLocationKind::Register, 12U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded && operation.mismatch_kind == NativeX64FlowOperandMismatchKind::InvalidRegister &&
            plan.mismatch_counts.invalid_register == 1U,
        "G14 did not distinguish an invalid/reserved GP register from an ordinary register mismatch");
}

void test_g14_classifies_flow_register_to_emitter_memory() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::FlowRegisterEmitterMemory &&
            plan.mismatch_counts.flow_register_emitter_memory == 1U,
        "G14 did not classify N112 register versus FunctionEmitter memory");
}

void test_g14_classifies_flow_memory_to_emitter_register() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U),
        make_location(NativeRegisterLocationKind::Register, 12U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::FlowMemoryEmitterRegister &&
            plan.mismatch_counts.flow_memory_emitter_register == 1U,
        "G14 did not classify N112 memory versus FunctionEmitter register");
}

void test_g14_classifies_physical_frame_displacement_mismatch() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U),
        true, true, -32, -40);
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::FrameDisplacementMismatch &&
            operation.flow_frame_displacement == -32 && operation.emitter_frame_displacement == -40 &&
            plan.mismatch_counts.frame_displacement == 1U,
        "G14 did not distinguish unequal physical RBP-relative frame displacements");
}

void test_g14_classifies_unavailable_frame_mapping() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 5U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::FrameMappingUnavailable &&
            plan.mismatch_counts.frame_mapping_unavailable == 1U,
        "G14 conflated an unavailable frame mapping with a proven displacement mismatch");
}

void test_g14_classifies_inconsistent_match_flag_as_evidence_disagreement() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 12U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded && operation.mismatch_kind == NativeX64FlowOperandMismatchKind::EvidenceDisagreement &&
            plan.mismatch_counts.evidence_disagreement == 1U,
        "G14 mislabeled equal physical locations with a false mismatch flag as a register-index mismatch");
}

void test_g14_classifies_unsupported_location_kind() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Temporary, 12U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::UnsupportedLocationKind &&
            plan.mismatch_counts.unsupported_location == 1U,
        "G14 did not fail closed and classify a temporary/unsupported Flow location");
}

void test_g14_real_function_reports_aggregate_mismatch_categories() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    const auto main_report = std::find_if(
        result.stats.native_n113_flow_edge_bundle_function_reports.begin(),
        result.stats.native_n113_flow_edge_bundle_function_reports.end(),
        [](const auto& report) { return report.function_name == "Main"; });
    require(result.succeeded() && main_report != result.stats.native_n113_flow_edge_bundle_function_reports.end(),
        "G14 could not find the real FunctionEmitter Main census");
    NativeX64FlowOperandMismatchCounts edge_sum;
    for (const auto& edge : main_report->edges) {
        edge_sum.register_index += edge.mismatch_counts.register_index;
        edge_sum.invalid_register += edge.mismatch_counts.invalid_register;
        edge_sum.flow_register_emitter_memory += edge.mismatch_counts.flow_register_emitter_memory;
        edge_sum.flow_memory_emitter_register += edge.mismatch_counts.flow_memory_emitter_register;
        edge_sum.frame_displacement += edge.mismatch_counts.frame_displacement;
        edge_sum.frame_mapping_unavailable += edge.mismatch_counts.frame_mapping_unavailable;
        edge_sum.evidence_disagreement += edge.mismatch_counts.evidence_disagreement;
        edge_sum.unsupported_location += edge.mismatch_counts.unsupported_location;
        edge_sum.register_class += edge.mismatch_counts.register_class;
    }
    require(edge_sum.register_index == main_report->mismatch_counts.register_index &&
            edge_sum.invalid_register == main_report->mismatch_counts.invalid_register &&
            edge_sum.flow_register_emitter_memory == main_report->mismatch_counts.flow_register_emitter_memory &&
            edge_sum.flow_memory_emitter_register == main_report->mismatch_counts.flow_memory_emitter_register &&
            edge_sum.frame_displacement == main_report->mismatch_counts.frame_displacement &&
            edge_sum.frame_mapping_unavailable == main_report->mismatch_counts.frame_mapping_unavailable &&
            edge_sum.evidence_disagreement == main_report->mismatch_counts.evidence_disagreement &&
            edge_sum.unsupported_location == main_report->mismatch_counts.unsupported_location &&
            edge_sum.register_class == main_report->mismatch_counts.register_class,
        "G14/G15 Function report mismatch counters do not equal its Edge report aggregation");
    require(std::any_of(main_report->edges.begin(), main_report->edges.end(), [](const auto& edge) {
        return std::any_of(edge.value_operand_evidence.begin(), edge.value_operand_evidence.end(),
            [](const auto& value) {
                return std::any_of(value.operations.begin(), value.operations.end(), [](const auto& operation) {
                    return operation.mismatch_kind != NativeX64FlowOperandMismatchKind::None;
                });
            });
    }), "G14 real FunctionEmitter census did not expose a categorized mismatch occurrence");
}

void test_g14_mismatch_classification_is_deterministic_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto first = compile_with_n113_g11_edge_eligibility(source, true);
    const auto second = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.succeeded() && first.succeeded() && second.succeeded() &&
            baseline.object_bytes == first.object_bytes && first.object_bytes == second.object_bytes &&
            first.stats.native_n113_flow_edge_bundle_function_reports.size() ==
                second.stats.native_n113_flow_edge_bundle_function_reports.size(),
        "G14 mismatch classification changed COFF bytes or report count across deterministic runs");
    for (std::size_t i = 0; i < first.stats.native_n113_flow_edge_bundle_function_reports.size(); ++i) {
        const auto& a = first.stats.native_n113_flow_edge_bundle_function_reports[i];
        const auto& b = second.stats.native_n113_flow_edge_bundle_function_reports[i];
        require(a.function_index == b.function_index && a.function_name == b.function_name &&
                a.edges.size() == b.edges.size(),
            "G14 report order or edge count is not deterministic");
        for (std::size_t e = 0; e < a.edges.size(); ++e) {
            require(a.edges[e].edge_id == b.edges[e].edge_id &&
                    a.edges[e].mismatch_counts.register_index == b.edges[e].mismatch_counts.register_index &&
                    a.edges[e].mismatch_counts.frame_displacement == b.edges[e].mismatch_counts.frame_displacement,
                "G14 categorized mismatch evidence changed between deterministic runs");
        }
    }
}

void test_g14_composes_with_g2_without_changing_machine_moves_or_bytes() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g11_edge_eligibility(source, false, true);
    const auto g2_plus_g14 = compile_with_n113_g11_edge_eligibility(source, true, true);
    require(g2_only.succeeded() && g2_plus_g14.succeeded() &&
            g2_plus_g14.stats.native_n113_emitter_actions_inserted ==
                g2_only.stats.native_n113_emitter_actions_inserted &&
            g2_plus_g14.stats.native_n113_emitter_machine_moves_inserted ==
                g2_only.stats.native_n113_emitter_machine_moves_inserted &&
            g2_plus_g14.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_only.stats.native_n113_emitter_transfer_bytes_inserted &&
            g2_plus_g14.object_bytes == g2_only.object_bytes,
        "G14 mismatch classification altered independent G2 transfers or emitted object bytes");
}

void test_g13_does_not_claim_insertion_readiness() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.succeeded() && result.stats.native_n113_flow_edge_bundle_candidate_edges ==
                result.stats.native_n113_flow_edge_bundle_proof_gate_passed +
                result.stats.native_n113_flow_edge_bundle_blocked &&
            result.stats.native_n113_emitter_actions_inserted == 0U &&
            result.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            result.stats.native_n113_emitter_transfer_bytes_inserted == 0U,
        "G13 value evidence incorrectly claimed or performed Edge Bundle insertion");
}

void test_g7_schedules_acyclic_gp_copies_deterministically() {
    const auto anchor = make_g7_test_anchor(41U, 1U, 2U);
    auto candidates = make_g7_candidate_plan({
        make_g7_candidate(41U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor),
        make_g7_candidate(41U, 1U, 2U, 8U,
            make_location(NativeRegisterLocationKind::Register, 9U),
            make_location(NativeRegisterLocationKind::Register, 10U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(plan.succeeded && plan.edges.size() == 1U && plan.scheduled_edges == 1U &&
            plan.candidate_copies == 2U && plan.scheduled_copies == 2U &&
            plan.scheduled_operations == 2U && plan.gp_cycle_temporaries_used == 0U,
        "G7 failed to schedule a deterministic acyclic GP edge-copy set");
    require(plan.edges[0].bundle.operations[0].value_id == 8U &&
            plan.edges[0].bundle.operations[1].value_id == 7U &&
            !plan.machine_insertion_ready && !plan.scratch_liveness_proven,
        "G7 did not preserve dependency-safe order or the planning-only safety boundary");
}

void test_g7_breaks_gp_parallel_copy_cycle_with_class_correct_temporary() {
    const auto anchor = make_g7_test_anchor(42U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(42U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor),
        make_g7_candidate(42U, 1U, 2U, 8U,
            make_location(NativeRegisterLocationKind::Register, 9U),
            make_location(NativeRegisterLocationKind::Register, 8U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 10U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(plan.succeeded && plan.gp_cycle_temporaries_used == 1U &&
            plan.edges[0].bundle.operations.size() == 3U &&
            plan.edges[0].bundle.operations[0].source.register_index == 8U &&
            plan.edges[0].bundle.operations[0].destination.kind == NativeRegisterLocationKind::Temporary &&
            plan.edges[0].bundle.operations[0].destination.register_index == 10U &&
            plan.edges[0].bundle.operations[1].source.register_index == 9U &&
            plan.edges[0].bundle.operations[1].destination.register_index == 8U &&
            plan.edges[0].bundle.operations[2].source.kind == NativeRegisterLocationKind::Temporary &&
            plan.edges[0].bundle.operations[2].destination.register_index == 9U,
        "G7 did not break a GP register cycle with the dedicated R10 temporary");
}

void test_g7_schedules_gp_and_xmm_classes_independently() {
    const auto anchor = make_g7_test_anchor(43U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(43U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor),
        make_g7_candidate(43U, 1U, 2U, 9U,
            make_location(NativeRegisterLocationKind::Register, 1U),
            make_location(NativeRegisterLocationKind::Register, 2U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {9U, NativeRegisterClass::FloatingPoint}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 4U));
    require(plan.succeeded && plan.scheduled_operations == 2U &&
            plan.edges[0].bundle.operations.size() == 2U &&
            plan.edges[0].bundle.operations[0].value_id == 7U &&
            plan.edges[0].bundle.operations[1].value_id == 9U &&
            plan.gp_cycle_temporaries_used == 0U && plan.xmm_cycle_temporaries_used == 0U,
        "G7 did not schedule GP and XMM copies independently in a deterministic order");
}

void test_g7_distinguishes_gp_and_xmm_registers_with_the_same_index() {
    const auto anchor = make_g7_test_anchor(51U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(51U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor),
        make_g7_candidate(51U, 1U, 2U, 9U,
            make_location(NativeRegisterLocationKind::Register, 1U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {9U, NativeRegisterClass::FloatingPoint}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(plan.succeeded && plan.scheduled_copies == 2U &&
            plan.edges[0].bundle.operations.size() == 2U,
        "G7 incorrectly treated GP R9 and XMM9 as the same physical destination");
}

void test_g7_keeps_memory_to_memory_copy_as_a_scheduled_operation() {
    const auto anchor = make_g7_test_anchor(44U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(44U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::SpillSlot, 0U, 3U),
            make_location(NativeRegisterLocationKind::HomeSlot, 0U, 6U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(plan.succeeded && plan.scheduled_operations == 1U &&
            plan.edges[0].bundle.operations[0].source.kind == NativeRegisterLocationKind::SpillSlot &&
            plan.edges[0].bundle.operations[0].destination.kind == NativeRegisterLocationKind::HomeSlot &&
            plan.gp_cycle_temporaries_used == 0U,
        "G7 should retain memory-to-memory moves for the later encoder expansion stage");
}

void test_g7_rejects_duplicate_destinations_across_one_edge() {
    const auto anchor = make_g7_test_anchor(45U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(45U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 10U), anchor),
        make_g7_candidate(45U, 1U, 2U, 8U,
            make_location(NativeRegisterLocationKind::Register, 9U),
            make_location(NativeRegisterLocationKind::Register, 10U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.back().code == "N113H003A",
        "G7 accepted multiple writes to one edge destination");
}

void test_g7_rejects_cycle_temporary_aliasing_an_edge_endpoint() {
    const auto anchor = make_g7_test_anchor(46U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(46U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 10U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor),
        make_g7_candidate(46U, 1U, 2U, 8U,
            make_location(NativeRegisterLocationKind::Register, 9U),
            make_location(NativeRegisterLocationKind::Register, 10U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 10U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H003F",
        "G7 accepted a cycle temporary that aliases a physical input edge register");
}

void test_g7_rejects_cycle_temporaries_outside_allocator_pools() {
    const auto anchor = make_g7_test_anchor(47U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(47U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}};
    const auto bad_gp = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 12U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    const auto bad_xmm = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 6U));
    require(!bad_gp.succeeded && bad_gp.diagnostics.front().code == "N113H0032" &&
            !bad_xmm.succeeded && bad_xmm.diagnostics.front().code == "N113H0033",
        "G7 accepted a cycle temporary outside the reserved allocator scratch pools");
}

void test_g7_rejects_inconsistent_branch_anchor_records_for_one_edge() {
    const auto anchor = make_g7_test_anchor(48U, 1U, 2U);
    auto other_anchor = anchor;
    other_anchor.instruction_offset += 1U;
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(48U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor),
        make_g7_candidate(48U, 1U, 2U, 8U,
            make_location(NativeRegisterLocationKind::Register, 9U),
            make_location(NativeRegisterLocationKind::Register, 10U), other_anchor)
    }, 1U);
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}, {8U, NativeRegisterClass::GeneralPurpose}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.back().code == "N113H0039",
        "G7 accepted inconsistent branch-anchor records for one FlowEdgeId");
}

void test_g7_rejects_candidate_plan_that_claims_machine_insertion_ready() {
    const auto anchor = make_g7_test_anchor(49U, 1U, 2U);
    auto candidates = make_g7_candidate_plan({
        make_g7_candidate(49U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor)
    }, 1U);
    candidates.ready_for_machine_insertion = true;
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {7U, NativeRegisterClass::GeneralPurpose}};
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, classes, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0030",
        "G7 accepted a G6 candidate plan that incorrectly claims insertion readiness");
}

void test_g7_rejects_missing_authoritative_value_class() {
    const auto anchor = make_g7_test_anchor(50U, 1U, 2U);
    const auto candidates = make_g7_candidate_plan({
        make_g7_candidate(50U, 1U, 2U, 7U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U), anchor)
    }, 1U);
    const auto plan = schedule_native_x64_flow_edge_transfer_candidates(
        candidates, {}, make_location(NativeRegisterLocationKind::Temporary, 11U),
        make_location(NativeRegisterLocationKind::Temporary, 5U));
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113H0035",
        "G7 accepted a candidate whose value class is not authoritative");
}

void test_g3_g2_object_keeps_valid_relocations_and_defined_entry_symbols() {
    const auto inserted = compile_with_n113_g2_insertion(n113_emitter_pressure_source(), true);
    const auto& object_file = inserted.native_object.object_file;
    require(!object_file.relocations.empty(),
        "G3 object must retain relocations after N113-G2 insertion");
    for (const auto& relocation : object_file.relocations) {
        const auto section = std::find_if(object_file.sections.begin(), object_file.sections.end(),
            [&relocation](const ObjectSection& item) { return item.id == relocation.section_id; });
        require(section != object_file.sections.end() && relocation.offset <= section->size &&
                relocation.width <= section->size - relocation.offset,
            "G3 object contains a relocation outside its rebased section bounds");
    }
    const auto find_source_symbol = [&](const std::string& name) {
        return std::find_if(inserted.native_object.symbol_table.symbols.begin(),
            inserted.native_object.symbol_table.symbols.end(), [&](const NativeSymbolRecord& symbol) {
                return symbol.canonical_source_name == name && !symbol.mangled_name.empty();
            });
    };
    const auto entry = find_source_symbol("main");
    const auto helper = find_source_symbol("helper");
    require(entry != inserted.native_object.symbol_table.symbols.end() &&
            helper != inserted.native_object.symbol_table.symbols.end(),
        "G3 inserted COFF object lost the defined entry or internal helper symbol");
    require(inserted.stats.native_n113_emitter_actions_inserted ==
                inserted.stats.native_n113_emitter_actions_rebound &&
            inserted.stats.native_n113_emitter_machine_moves_inserted >=
                inserted.stats.native_n113_emitter_actions_inserted,
        "G3 inserted object did not preserve exactly-once action materialization accounting");
}

} // namespace


void test_g15_classifies_register_class_mismatch_with_equal_register_index() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 12U),
        false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    const auto& edge = plan.edges.front();
    const auto& value = edge.value_operand_evidence.front();
    const auto& operation = value.operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::RegisterClassMismatch &&
            operation.flow_register_class == NativeRegisterClass::FloatingPoint &&
            value.register_class == NativeRegisterClass::GeneralPurpose &&
            !operation.location_matches &&
            plan.mismatch_counts.register_class == 1U &&
            edge.mismatch_counts.register_class == 1U &&
            value.mismatch_counts.register_class == 1U,
        "G15 did not preserve and classify a Flow Operation register-class mismatch");
}

void test_g15_register_class_mismatch_precedes_register_index_difference() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 13U),
        false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::RegisterClassMismatch &&
            plan.mismatch_counts.register_class == 1U &&
            plan.mismatch_counts.register_index == 0U,
        "G15 incorrectly classified a class mismatch as a register-index mismatch");
}

void test_g15_register_class_mismatch_precedes_register_memory_difference() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U),
        false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::RegisterClassMismatch &&
            plan.mismatch_counts.register_class == 1U &&
            plan.mismatch_counts.flow_register_emitter_memory == 0U,
        "G15 allowed a register/memory mismatch to mask the authoritative class mismatch");
}

void test_g15_matching_register_classes_keep_g14_location_categories() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 13U));
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(plan.succeeded &&
            operation.mismatch_kind == NativeX64FlowOperandMismatchKind::RegisterIndexMismatch &&
            plan.mismatch_counts.register_class == 0U &&
            plan.mismatch_counts.register_index == 1U,
        "G15 changed G14 register-index classification when register classes agree");
}

void test_g15_operation_report_exposes_flow_and_candidate_classes() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 12U),
        false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    const auto& value = plan.edges.front().value_operand_evidence.front();
    const auto& operation = value.operations.front();
    require(operation.flow_register_class == NativeRegisterClass::FloatingPoint &&
            value.register_class == NativeRegisterClass::GeneralPurpose,
        "G15 did not expose actual Flow and candidate Value register classes separately");
}

void test_g15_class_mismatch_counts_reconcile_value_edge_and_function() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 12U),
        false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    const auto& edge = plan.edges.front();
    const auto& value = edge.value_operand_evidence.front();
    require(plan.succeeded && edge.mismatch_counts.register_class == 1U &&
            value.mismatch_counts.register_class == 1U &&
            plan.mismatch_counts.register_class == 1U,
        "G15 register-class mismatch occurrence was not counted consistently at all levels");
}

void test_g15_class_mismatch_is_distinct_from_invalid_register() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 4U),
        make_location(NativeRegisterLocationKind::Register, 4U),
        false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    const auto& operation = plan.edges.front().value_operand_evidence.front().operations.front();
    require(operation.mismatch_kind == NativeX64FlowOperandMismatchKind::RegisterClassMismatch &&
            plan.mismatch_counts.register_class == 1U &&
            plan.mismatch_counts.invalid_register == 0U,
        "G15 conflated a class mismatch with invalid register-index evidence");
}

void test_g15_register_class_mismatch_is_deterministic() {
    auto make_plan = [] {
        return make_g14_single_mismatch_plan(
            make_location(NativeRegisterLocationKind::Register, 12U),
            make_location(NativeRegisterLocationKind::Register, 12U),
            false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    };
    const auto first = make_plan();
    const auto second = make_plan();
    const auto& a = first.edges.front().value_operand_evidence.front().operations.front();
    const auto& b = second.edges.front().value_operand_evidence.front().operations.front();
    require(first.succeeded && second.succeeded &&
            a.mismatch_kind == NativeX64FlowOperandMismatchKind::RegisterClassMismatch &&
            b.mismatch_kind == a.mismatch_kind &&
            a.flow_register_class == b.flow_register_class &&
            first.mismatch_counts.register_class == second.mismatch_counts.register_class,
        "G15 class-mismatch evidence is not deterministic");
}

void test_g15_classification_remains_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto audited = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.succeeded() && audited.succeeded() &&
            baseline.object_bytes == audited.object_bytes,
        "G15 register-class classification changed emitted COFF bytes");
}

void test_g15_classification_composes_with_g2_without_changing_moves() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g11_edge_eligibility(source, false, true);
    const auto g2_plus = compile_with_n113_g11_edge_eligibility(source, true, true);
    require(g2_only.succeeded() && g2_plus.succeeded() &&
            g2_only.object_bytes == g2_plus.object_bytes &&
            g2_only.stats.native_n113_emitter_machine_moves_inserted ==
                g2_plus.stats.native_n113_emitter_machine_moves_inserted &&
            g2_only.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_plus.stats.native_n113_emitter_transfer_bytes_inserted,
        "G15 classification changed independent G2 machine moves or object bytes");
}

void test_g15_does_not_claim_edge_bundle_insertion_readiness() {
    const auto plan = make_g14_single_mismatch_plan(
        make_location(NativeRegisterLocationKind::Register, 12U),
        make_location(NativeRegisterLocationKind::Register, 12U),
        false, false, 0, 0, NativeRegisterClass::FloatingPoint);
    require(plan.succeeded && plan.proof_gate_passed_edges == 0U &&
            plan.blocked_edges == 1U,
        "G15 mismatch classification incorrectly cleared an Edge Bundle eligibility blocker");
}

void test_g15_g10_crosswalk_preserves_register_class_mismatch_as_evidence() {
    auto fixture = make_g4_flow_edge_fixture();
    for (auto& operation : fixture.flow_segments.operations) {
        for (auto& value : operation.before_locations) {
            if (value.value_id == 7U) value.register_class = NativeRegisterClass::FloatingPoint;
        }
        for (auto& value : operation.after_locations) {
            if (value.value_id == 7U) value.register_class = NativeRegisterClass::FloatingPoint;
        }
    }
    const auto crosswalk = audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U,
        make_g8_operand_anchors(fixture));
    require(crosswalk.succeeded && crosswalk.operand_operations_audited == 3U &&
            crosswalk.operand_location_mismatches == 3U && !crosswalk.all_operands_owned &&
            std::all_of(crosswalk.operands.begin(), crosswalk.operands.end(), [](const auto& operand) {
                return operand.register_class == NativeRegisterClass::FloatingPoint &&
                    !operand.location_matches;
            }) &&
            std::any_of(crosswalk.diagnostics.begin(), crosswalk.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.code == "N113J0016";
            }),
        "G15 G10 crosswalk discarded or failed to classify Flow/N111 register-class disagreement");

    const auto eligibility = audit_native_x64_flow_edge_bundle_eligibility(
        make_g11_candidate_plan(), make_g11_scratch_plan(), crosswalk,
        make_g11_value_classes(), 0U);
    require(eligibility.succeeded && eligibility.candidate_edges == 1U &&
            eligibility.blocked_edges == 1U && eligibility.proof_gate_passed_edges == 0U &&
            eligibility.mismatch_counts.register_class == 3U &&
            eligibility.edges.front().mismatch_counts.register_class == 3U,
        "G15 did not carry the G10 register-class mismatch into the exact-edge eligibility blocker");
}

SourceSpan g16_source_span(std::size_t byte_offset, std::uint32_t line,
                           std::uint32_t column, SourceFileId source_id = 9U) {
    SourceSpan span;
    span.source_id = source_id;
    span.start = SourcePosition{byte_offset, line, column};
    span.end = SourcePosition{byte_offset + 3U, line, column + 3U};
    return span;
}

void assign_g16_fixture_source_spans(G4FlowEdgeFixture& fixture) {
    fixture.function.register_ir.blocks[0U].instructions[0U].source_span =
        g16_source_span(10U, 2U, 4U);
    fixture.function.register_ir.blocks[1U].instructions[0U].source_span =
        g16_source_span(40U, 5U, 8U);
    fixture.function.register_ir.blocks[2U].instructions[0U].source_span =
        g16_source_span(80U, 9U, 3U);
}

NativeX64FlowOperationOperandOwnershipPlan audit_g16_fixture(
    const G4FlowEdgeFixture& fixture,
    const std::vector<X64FlowEmitterAnchor>& anchors) {
    return audit_native_x64_flow_operation_operand_ownership(
        fixture.function, fixture.flow_segments, fixture.emitter_locations,
        {{0U, -8}, {1U, -16}}, fixture.flow_frame, 0U, anchors);
}

const NativeX64FlowOperationOperandOwnershipEntry& find_g16_operand(
    const NativeX64FlowOperationOperandOwnershipPlan& plan,
    NativeRegisterInstructionOriginKind origin_kind) {
    const auto found = std::find_if(plan.operands.begin(), plan.operands.end(),
        [origin_kind](const auto& entry) { return entry.flow_origin_kind == origin_kind; });
    if (found == plan.operands.end())
        throw std::runtime_error("G16 test fixture is missing an operand with the requested Flow origin kind");
    return *found;
}

void test_g16_flow_read_reports_before_position_and_source_span() {
    auto fixture = make_g4_flow_edge_fixture();
    assign_g16_fixture_source_spans(fixture);
    const auto anchors = make_g8_operand_anchors(fixture);
    const auto plan = audit_g16_fixture(fixture, anchors);
    const auto& read = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowRead);
    require(plan.succeeded && read.operand_kind == X64FlowEmitterAnchor::OperandKind::ValueRead &&
            read.flow_position_kind == NativeX64FlowOperandPositionKind::BeforeOperation &&
            read.source_span_matches_emitter_anchor && read.source_span.source_id == 9U &&
            read.source_span.start.line == 9U && read.source_span.start.column == 3U,
        "G16 did not retain FlowRead role, N112 before-position and exact source span");
}

void test_g16_flow_write_reports_after_position() {
    auto fixture = make_g4_flow_edge_fixture();
    assign_g16_fixture_source_spans(fixture);
    const auto anchors = make_g8_operand_anchors(fixture);
    const auto plan = audit_g16_fixture(fixture, anchors);
    const auto& write = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowWrite);
    require(plan.succeeded && write.operand_kind == X64FlowEmitterAnchor::OperandKind::ValueWrite &&
            write.flow_position_kind == NativeX64FlowOperandPositionKind::AfterOperation &&
            write.source_span_matches_emitter_anchor && write.source_span.start.line == 5U,
        "G16 did not retain FlowWrite role and N112 after-position");
}

void test_g16_flow_declare_reports_after_position() {
    auto fixture = make_g4_flow_edge_fixture();
    assign_g16_fixture_source_spans(fixture);
    const auto anchors = make_g8_operand_anchors(fixture);
    const auto plan = audit_g16_fixture(fixture, anchors);
    const auto& declaration = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowDeclare);
    require(plan.succeeded && declaration.operand_kind == X64FlowEmitterAnchor::OperandKind::ValueDeclare &&
            declaration.flow_position_kind == NativeX64FlowOperandPositionKind::AfterOperation &&
            declaration.source_span_matches_emitter_anchor && declaration.source_span.start.line == 2U,
        "G16 did not retain FlowDeclare role and N112 after-position");
}

void test_g16_address_read_reports_before_position_and_role() {
    auto fixture = make_g4_flow_edge_fixture();
    assign_g16_fixture_source_spans(fixture);
    fixture.emitter_locations.values.at(7U).address_taken = true;
    fixture.emitter_locations.values.at(7U).active_location =
        fixture.emitter_locations.values.at(7U).canonical_home;
    auto anchors = make_g8_operand_anchors(fixture);
    const auto read_anchor = std::find_if(anchors.begin(), anchors.end(), [](const auto& anchor) {
        return anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowRead;
    });
    require(read_anchor != anchors.end(), "G16 AddressRead fixture has no FlowRead anchor");
    read_anchor->emitted_operand_kind = X64FlowEmitterAnchor::OperandKind::AddressRead;
    read_anchor->emitted_operand_location = fixture.emitter_locations.values.at(7U).canonical_home;
    const auto plan = audit_g16_fixture(fixture, anchors);
    const auto& read = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowRead);
    require(plan.succeeded && read.operand_kind == X64FlowEmitterAnchor::OperandKind::AddressRead &&
            read.flow_position_kind == NativeX64FlowOperandPositionKind::BeforeOperation &&
            read.source_span_matches_emitter_anchor,
        "G16 failed to distinguish AddressRead from ValueRead or selected the wrong N112 position");
}

void test_g16_reports_candidate_and_actual_flow_register_classes() {
    auto fixture = make_g4_flow_edge_fixture();
    for (auto& operation : fixture.flow_segments.operations) {
        for (auto& value : operation.before_locations)
            if (value.value_id == 7U) value.register_class = NativeRegisterClass::FloatingPoint;
        for (auto& value : operation.after_locations)
            if (value.value_id == 7U) value.register_class = NativeRegisterClass::FloatingPoint;
    }
    const auto plan = audit_g16_fixture(fixture, make_g8_operand_anchors(fixture));
    const auto& read = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowRead);
    require(plan.succeeded && read.register_class == NativeRegisterClass::FloatingPoint &&
            read.candidate_value_register_class == NativeRegisterClass::GeneralPurpose &&
            !read.location_matches,
        "G16 did not expose actual Flow and expected N111 Value classes in the operation crosswalk");
}

void test_g16_source_span_disagreement_is_visible_but_nonfatal() {
    auto fixture = make_g4_flow_edge_fixture();
    assign_g16_fixture_source_spans(fixture);
    auto anchors = make_g8_operand_anchors(fixture);
    const auto read_anchor = std::find_if(anchors.begin(), anchors.end(), [](const auto& anchor) {
        return anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowRead;
    });
    require(read_anchor != anchors.end(), "G16 source-span fixture has no FlowRead anchor");
    ++read_anchor->source_span.start.line;
    const auto plan = audit_g16_fixture(fixture, anchors);
    const auto& read = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowRead);
    require(plan.succeeded && !read.source_span_matches_emitter_anchor &&
            read.flow_operation_id != InvalidNativeFlowOperationId &&
            read.flow_position_kind == NativeX64FlowOperandPositionKind::BeforeOperation,
        "G16 failed to surface a source-span disagreement without discarding the operation evidence");
}

void test_g16_function_edge_reports_carry_operation_provenance() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    std::uint32_t operation_count = 0U;
    bool saw_read = false;
    bool saw_write = false;
    for (const auto& function : result.stats.native_n113_flow_edge_bundle_function_reports) {
        for (const auto& edge : function.edges) {
            for (const auto& value : edge.value_operand_evidence) {
                for (const auto& operation : value.operations) {
                    ++operation_count;
                    const auto expected_position = operation.flow_origin_kind == NativeRegisterInstructionOriginKind::FlowRead
                        ? NativeX64FlowOperandPositionKind::BeforeOperation
                        : NativeX64FlowOperandPositionKind::AfterOperation;
                    require(operation.flow_origin_kind != NativeRegisterInstructionOriginKind::Synthetic &&
                            operation.flow_position_kind == expected_position &&
                            operation.candidate_value_register_class == value.register_class &&
                            operation.source_span_matches_emitter_anchor,
                        "G16 FunctionEmitter report lost Flow role, N112 position, class or source anchor provenance");
                    saw_read = saw_read || operation.flow_origin_kind == NativeRegisterInstructionOriginKind::FlowRead;
                    saw_write = saw_write || operation.flow_origin_kind == NativeRegisterInstructionOriginKind::FlowWrite;
                }
            }
        }
    }
    require(result.succeeded() && operation_count > 0U && saw_read && saw_write,
        "G16 FunctionEmitter census did not expose read/write operation provenance");
}

void test_g17_function_report_preserves_actual_flow_register_class() {
    const std::string source =
        "module N113G17FloatEdges; "
        "public function Main() returns double { "
        "double x=1.0; double y=2.0; double sum=0.0; int i=0; "
        "while (i < 4) { if (i < 2) { sum = sum + x; } "
        "else { sum = sum + y; } i = i + 1; } return sum; }";
    const auto result = compile_with_n113_g11_edge_eligibility(source, true);
    std::uint32_t fp_operations = 0U;
    std::uint32_t edge_count = 0U;
    std::uint32_t value_count = 0U;
    std::uint32_t operation_count = 0U;
    for (const auto& function : result.stats.native_n113_flow_edge_bundle_function_reports) {
        for (const auto& edge : function.edges) {
            ++edge_count;
            for (const auto& value : edge.value_operand_evidence) {
                ++value_count;
                for (const auto& operation : value.operations) {
                    ++operation_count;
                    if (operation.flow_register_class == NativeRegisterClass::FloatingPoint) {
                        ++fp_operations;
                        require(value.register_class == operation.candidate_value_register_class,
                            "G17 FunctionEmitter report detached the candidate class from its ValueId");
                        require(operation.flow_origin_kind != NativeRegisterInstructionOriginKind::Synthetic,
                            "G17 FunctionEmitter report attached a Flow class to a synthetic operation");
                    }
                }
            }
        }
    }
    const std::string detail = "G17 public FunctionEmitter census lost actual Flow FP class: functions=" +
        std::to_string(result.stats.native_n113_flow_edge_bundle_function_reports.size()) +
        " edges=" + std::to_string(edge_count) + " values=" + std::to_string(value_count) +
        " operations=" + std::to_string(operation_count) + " flow_fp=" +
        std::to_string(fp_operations);
    require(result.succeeded() && fp_operations > 0U, detail.c_str());
}

void test_g17_flow_and_candidate_register_classes_are_both_projected() {
    const std::string source =
        "module N113G17Projection; "
        "public function Main() returns double { "
        "double x=1.0; double y=2.0; double sum=0.0; int i=0; "
        "while (i < 4) { if (i < 2) { sum = sum + x; } "
        "else { sum = sum + y; } i = i + 1; } return sum; }";
    const auto first = compile_with_n113_g11_edge_eligibility(source, true);
    const auto second = compile_with_n113_g11_edge_eligibility(source, true);
    require(first.succeeded() && second.succeeded() &&
            first.stats.native_n113_flow_edge_bundle_function_reports.size() ==
                second.stats.native_n113_flow_edge_bundle_function_reports.size(),
        "G17 could not repeat the FunctionEmitter class-projection audit");
    std::uint32_t compared = 0U;
    for (std::size_t f = 0; f < first.stats.native_n113_flow_edge_bundle_function_reports.size(); ++f) {
        const auto& a = first.stats.native_n113_flow_edge_bundle_function_reports[f];
        const auto& b = second.stats.native_n113_flow_edge_bundle_function_reports[f];
        require(a.function_index == b.function_index && a.edges.size() == b.edges.size(),
            "G17 FunctionEmitter report identity/order changed between runs");
        for (std::size_t e = 0; e < a.edges.size(); ++e) {
            const auto& av = a.edges[e].value_operand_evidence;
            const auto& bv = b.edges[e].value_operand_evidence;
            require(a.edges[e].edge_id == b.edges[e].edge_id && av.size() == bv.size(),
                "G17 edge/value evidence shape changed between runs");
            for (std::size_t v = 0; v < av.size(); ++v) {
                require(av[v].value_id == bv[v].value_id && av[v].operations.size() == bv[v].operations.size(),
                    "G17 operation evidence identity changed between runs");
                for (std::size_t o = 0; o < av[v].operations.size(); ++o) {
                    const auto& x = av[v].operations[o];
                    const auto& y = bv[v].operations[o];
                    require(x.flow_operation_id == y.flow_operation_id &&
                            x.flow_register_class == y.flow_register_class &&
                            x.candidate_value_register_class == y.candidate_value_register_class &&
                            x.candidate_value_register_class == av[v].register_class,
                        "G17 actual Flow/candidate register classes were dropped or became nondeterministic in the public report");
                    ++compared;
                }
            }
        }
    }
    require(compared > 0U, "G17 class projection test did not inspect any public operation evidence");
}

void test_g16_provenance_is_deterministic() {
    const auto source = n113_emitter_pressure_source();
    const auto first = compile_with_n113_g11_edge_eligibility(source, true);
    const auto second = compile_with_n113_g11_edge_eligibility(source, true);
    require(first.succeeded() && second.succeeded() &&
            first.stats.native_n113_flow_edge_bundle_function_reports.size() ==
                second.stats.native_n113_flow_edge_bundle_function_reports.size(),
        "G16 deterministic provenance fixture did not compile consistently");
    for (std::size_t f = 0; f < first.stats.native_n113_flow_edge_bundle_function_reports.size(); ++f) {
        const auto& a = first.stats.native_n113_flow_edge_bundle_function_reports[f];
        const auto& b = second.stats.native_n113_flow_edge_bundle_function_reports[f];
        require(a.function_index == b.function_index && a.edges.size() == b.edges.size(),
            "G16 function/edge report order changed between runs");
        for (std::size_t e = 0; e < a.edges.size(); ++e) {
            const auto& av = a.edges[e].value_operand_evidence;
            const auto& bv = b.edges[e].value_operand_evidence;
            require(a.edges[e].edge_id == b.edges[e].edge_id && av.size() == bv.size(),
                "G16 Value evidence order changed between runs");
            for (std::size_t v = 0; v < av.size(); ++v) {
                require(av[v].value_id == bv[v].value_id && av[v].operations.size() == bv[v].operations.size(),
                    "G16 Operation evidence order changed between runs");
                for (std::size_t o = 0; o < av[v].operations.size(); ++o) {
                    const auto& x = av[v].operations[o];
                    const auto& y = bv[v].operations[o];
                    require(x.flow_operation_id == y.flow_operation_id &&
                            x.flow_origin_kind == y.flow_origin_kind &&
                            x.flow_position_kind == y.flow_position_kind &&
                            x.source_span_matches_emitter_anchor == y.source_span_matches_emitter_anchor &&
                            x.source_span.start.byte_offset == y.source_span.start.byte_offset &&
                            x.source_span.start.line == y.source_span.start.line &&
                            x.source_span.start.column == y.source_span.start.column,
                        "G16 source operation provenance changed between deterministic runs");
                }
            }
        }
    }
}

void test_g16_provenance_is_opt_in_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto audited = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.succeeded() && audited.succeeded() &&
            baseline.stats.native_n113_flow_edge_bundle_function_reports.empty() &&
            !audited.stats.native_n113_flow_edge_bundle_function_reports.empty() &&
            baseline.object_bytes == audited.object_bytes,
        "G16 provenance changed default behavior or emitted COFF bytes");
}

void test_g16_provenance_composes_with_g2_without_changing_moves() {
    const auto source = n113_emitter_pressure_source();
    const auto g2_only = compile_with_n113_g11_edge_eligibility(source, false, true);
    const auto g2_plus = compile_with_n113_g11_edge_eligibility(source, true, true);
    require(g2_only.succeeded() && g2_plus.succeeded() &&
            g2_only.object_bytes == g2_plus.object_bytes &&
            g2_only.stats.native_n113_emitter_machine_moves_inserted ==
                g2_plus.stats.native_n113_emitter_machine_moves_inserted &&
            g2_only.stats.native_n113_emitter_transfer_bytes_inserted ==
                g2_plus.stats.native_n113_emitter_transfer_bytes_inserted,
        "G16 source provenance changed independent G2 inserted machine moves or object bytes");
}

void test_g16_provenance_does_not_promote_insertion_readiness() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.succeeded() &&
            result.stats.native_n113_emitter_actions_inserted == 0U &&
            result.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            result.stats.native_n113_emitter_transfer_bytes_inserted == 0U,
        "G16 context reporting inserted transfers or promoted Edge Bundle insertion readiness");
}

void test_g16_operation_evidence_exposes_exact_candidate_class_separately() {
    auto fixture = make_g4_flow_edge_fixture();
    assign_g16_fixture_source_spans(fixture);
    auto anchors = make_g8_operand_anchors(fixture);
    const auto plan = audit_g16_fixture(fixture, anchors);
    const auto& read = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowRead);
    require(plan.succeeded && read.register_class == NativeRegisterClass::GeneralPurpose &&
            read.candidate_value_register_class == NativeRegisterClass::GeneralPurpose &&
            read.source_span_matches_emitter_anchor && read.source_span.start.byte_offset == 80U,
        "G16 operation evidence does not preserve candidate class and exact source position independently");
}

void test_g19_g8_exports_every_live_value_endpoint_and_reconciles_counts() {
    auto fixture = make_g4_flow_edge_fixture();
    // R10 is an actual Flow endpoint in this fixture (merge-block Value 7).
    // Match the established G8 fixture convention and keep the dedicated
    // cycle temporary on R11 so G7 can build the intended parallel-copy plan.
    const auto gp = make_location(NativeRegisterLocationKind::Temporary, 11U);
    const auto xmm = make_location(NativeRegisterLocationKind::Temporary, 5U);
    const auto artifacts = make_g8_test_artifacts(fixture, gp, xmm);
    const auto proof = audit_native_x64_flow_edge_schedule_readiness(
        fixture.function, fixture.analysis.liveness, artifacts.edge_audit, artifacts.schedule,
        fixture.emitter_locations, artifacts.emitter_local_home_offsets, fixture.flow_frame,
        artifacts.operation_anchors, artifacts.branch_anchors, gp, xmm, {}, {});
    require(proof.succeeded && !proof.edges.empty(),
        "G19 G8 fixture did not produce a successful exact-edge readiness audit");
    std::uint32_t audited_values = 0U;
    std::uint32_t matched_endpoints = 0U;
    std::uint32_t mismatched_endpoints = 0U;
    for (const auto& edge : proof.edges) {
        require(edge.endpoint_value_evidence.size() == edge.live_values_audited,
            "G19 did not export exactly one endpoint record per G8 live-through Value");
        std::set<std::uint32_t> value_ids;
        std::uint32_t edge_matches = 0U;
        std::uint32_t edge_mismatches = 0U;
        for (const auto& value : edge.endpoint_value_evidence) {
            require(value.value_id != 0U && value_ids.insert(value.value_id).second,
                "G19 endpoint evidence contains an invalid or duplicate ValueId on one exact edge");
            require(value.function_operand_anchor_count <= proof.operand_anchors_validated,
                "G19 per-Value operand anchor count exceeds the function audit total");
            if (value.predecessor_matches_emitter) ++edge_matches;
            else ++edge_mismatches;
            if (value.successor_matches_emitter) ++edge_matches;
            else ++edge_mismatches;
            if (value.predecessor_uses_frame)
                require(value.predecessor_frame_displacement < 0 &&
                        (value.predecessor_frame_displacement % 8) == 0,
                    "G19 predecessor frame evidence is not a valid negative aligned RBP displacement");
            if (value.successor_uses_frame)
                require(value.successor_frame_displacement < 0 &&
                        (value.successor_frame_displacement % 8) == 0,
                    "G19 successor frame evidence is not a valid negative aligned RBP displacement");
            if (value.emitter_location_is_home_slot && value.emitter_frame_mapping_available)
                require(value.emitter_frame_displacement < 0 &&
                        (value.emitter_frame_displacement % 8) == 0,
                    "G19 Emitter home mapping is not a valid negative aligned RBP displacement");
            if (!value.emitter_location_is_home_slot)
                require(!value.emitter_frame_mapping_available && value.emitter_frame_displacement == 0,
                    "G19 non-home Emitter location unexpectedly carries a frame displacement");
        }
        require(edge_matches == edge.endpoint_ownership_matches &&
                edge_mismatches == edge.endpoint_ownership_mismatches &&
                edge_matches + edge_mismatches == edge.live_values_audited * 2U,
            "G19 endpoint match/mismatch counts do not reconcile with the two endpoints per Value");
        audited_values += edge.live_values_audited;
        matched_endpoints += edge_matches;
        mismatched_endpoints += edge_mismatches;
    }
    require(audited_values == proof.live_edge_values_audited &&
            matched_endpoints == proof.endpoint_ownership_matches &&
            mismatched_endpoints == proof.endpoint_ownership_mismatches,
        "G19 per-edge endpoint records do not reconcile with aggregate G8 counters");
}

void test_g19_function_emitter_report_projects_endpoint_records() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    std::uint32_t edge_count = 0U;
    std::uint32_t endpoint_records = 0U;
    bool saw_mismatch = false;
    for (const auto& function : result.stats.native_n113_flow_edge_bundle_function_reports) {
        for (const auto& edge : function.edges) {
            ++edge_count;
            require(edge.live_values_audited == edge.endpoint_value_evidence.size(),
                "G19 public FunctionEmitter report lost or duplicated a live-Value endpoint record");
            std::uint32_t matches = 0U;
            std::uint32_t mismatches = 0U;
            std::set<std::uint32_t> value_ids;
            for (const auto& value : edge.endpoint_value_evidence) {
                require(value.value_id != 0U && value_ids.insert(value.value_id).second,
                    "G19 public report contains duplicate or invalid endpoint ValueIds on an edge");
                matches += value.predecessor_matches_emitter ? 1U : 0U;
                matches += value.successor_matches_emitter ? 1U : 0U;
                mismatches += value.predecessor_matches_emitter ? 0U : 1U;
                mismatches += value.successor_matches_emitter ? 0U : 1U;
                saw_mismatch = saw_mismatch || !value.predecessor_matches_emitter ||
                    !value.successor_matches_emitter;
                ++endpoint_records;
            }
            require(matches == edge.endpoint_ownership_matches &&
                    mismatches == edge.endpoint_ownership_mismatches &&
                    matches + mismatches == edge.live_values_audited * 2U,
                "G19 public FunctionEmitter endpoint counters do not reconcile with Value records");
        }
    }
    require(result.succeeded() && edge_count > 0U && endpoint_records > 0U && saw_mismatch &&
            result.stats.native_n113_emitter_actions_inserted == 0U &&
            result.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            result.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "G19 public endpoint projection failed or changed the no-insertion/readiness state");
}

void test_g19_endpoint_projection_is_deterministic_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto first = compile_with_n113_g11_edge_eligibility(source, true);
    const auto second = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.succeeded() && first.succeeded() && second.succeeded() &&
            baseline.object_bytes == first.object_bytes && first.object_bytes == second.object_bytes,
        "G19 read-only endpoint evidence changed emitted COFF bytes");
    require(first.stats.native_n113_flow_edge_bundle_function_reports.size() ==
            second.stats.native_n113_flow_edge_bundle_function_reports.size(),
        "G19 function report count changed between deterministic runs");
    for (std::size_t f = 0U; f < first.stats.native_n113_flow_edge_bundle_function_reports.size(); ++f) {
        const auto& a = first.stats.native_n113_flow_edge_bundle_function_reports[f];
        const auto& b = second.stats.native_n113_flow_edge_bundle_function_reports[f];
        require(a.function_index == b.function_index && a.function_name == b.function_name &&
                a.edges.size() == b.edges.size(),
            "G19 function or edge report identity/order changed between runs");
        for (std::size_t e = 0U; e < a.edges.size(); ++e) {
            const auto& x = a.edges[e];
            const auto& y = b.edges[e];
            require(x.edge_id == y.edge_id && x.predecessor_block_id == y.predecessor_block_id &&
                    x.successor_block_id == y.successor_block_id &&
                    x.endpoint_value_evidence.size() == y.endpoint_value_evidence.size(),
                "G19 exact-edge endpoint report identity/order changed between runs");
            for (std::size_t v = 0U; v < x.endpoint_value_evidence.size(); ++v) {
                const auto& p = x.endpoint_value_evidence[v];
                const auto& q = y.endpoint_value_evidence[v];
                const auto same_location = [](const NativeRegisterLocation& left,
                                              const NativeRegisterLocation& right) {
                    return left.kind == right.kind && left.register_index == right.register_index &&
                        left.spill_slot == right.spill_slot;
                };
                require(p.value_id == q.value_id && p.register_class == q.register_class &&
                        same_location(p.predecessor_flow_location, q.predecessor_flow_location) &&
                        same_location(p.successor_flow_location, q.successor_flow_location) &&
                        same_location(p.emitter_active_location, q.emitter_active_location) &&
                        p.predecessor_uses_frame == q.predecessor_uses_frame &&
                        p.predecessor_frame_displacement == q.predecessor_frame_displacement &&
                        p.successor_uses_frame == q.successor_uses_frame &&
                        p.successor_frame_displacement == q.successor_frame_displacement &&
                        p.emitter_location_is_home_slot == q.emitter_location_is_home_slot &&
                        p.emitter_frame_mapping_available == q.emitter_frame_mapping_available &&
                        p.emitter_frame_displacement == q.emitter_frame_displacement &&
                        p.predecessor_matches_emitter == q.predecessor_matches_emitter &&
                        p.successor_matches_emitter == q.successor_matches_emitter &&
                        p.function_operand_anchor_count == q.function_operand_anchor_count,
                    "G19 per-Value endpoint evidence changed between identical compiles");
            }
        }
    }
    require(first.stats.native_n113_emitter_actions_inserted == 0U &&
            first.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            first.stats.native_n113_emitter_transfer_bytes_inserted == 0U &&
            first.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "G19 endpoint projection inserted transfers or promoted machine-insertion readiness");
}

void test_g20_g4_live_values_reconcile_with_g6_candidates() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.succeeded() &&
            result.stats.native_n113_flow_edge_diagnostic_function_reports.size() > 0U,
        "G20 FunctionEmitter diagnostic report was not produced");
    const auto main_function = std::find_if(
        result.stats.native_n113_flow_edge_diagnostic_function_reports.begin(),
        result.stats.native_n113_flow_edge_diagnostic_function_reports.end(),
        [](const auto& item) { return item.function_name == "Main"; });
    require(main_function != result.stats.native_n113_flow_edge_diagnostic_function_reports.end(),
        "G20 Main diagnostic function report was not found");
    const auto& function = *main_function;
    require(function.g4_plan_succeeded && function.g6_plan_succeeded && function.g9_plan_succeeded &&
            function.g10_plan_succeeded && function.g11_plan_succeeded && function.edges.size() == 6U,
        "G20 did not preserve the successful G4/G6/G9/G10/G11 plan inventory");
    require(function.g4_edges_audited >= function.edges.size() &&
            function.g4_live_values_audited >= function.g6_candidate_values &&
            function.g6_candidate_edges == function.edges.size(),
        "G20 function-level G4/G6 counts do not reconcile with exact candidate-edge reports");

    std::uint32_t edge3_live_values = 0U;
    std::uint32_t other_edge_live_values = 0U;
    for (const auto& edge : function.edges) {
        require(edge.g4_edge_audited && edge.g4_live_values >= edge.g6_candidate_values &&
                edge.g6_candidate_values > 0U,
            "G20 did not retain the G4 live-through universe for a G6 candidate edge");
        std::set<std::uint32_t> g4_ids;
        for (const auto& value : edge.g4_values) {
            require(value.value_id != 0U && g4_ids.insert(value.value_id).second,
                "G20 G4 per-edge live-through records contain an invalid or duplicate ValueId");
        }
        std::set<std::uint32_t> g6_ids;
        for (const auto& candidate : edge.g6_candidates) {
            require(candidate.value_id != 0U && g6_ids.insert(candidate.value_id).second &&
                    g4_ids.count(candidate.value_id) == 1U,
                "G20 G6 candidate ValueId was not reconciled to its G4 live-through record");
            const auto g4_value = std::find_if(edge.g4_values.begin(), edge.g4_values.end(),
                [&](const auto& value) { return value.value_id == candidate.value_id; });
            require(g4_value != edge.g4_values.end() && g4_value->flow_location_changes &&
                    g4_value->is_g6_candidate,
                "G20 G6 candidate is not a G4 location-changing live-through Value");
        }
        require(edge.g6_branch_anchor_present && edge.g6_branch_anchor_identity_exact,
            "G20 lost the exact G6 branch-anchor identity already carried by its candidates");
        if (edge.edge_id == 3U) edge3_live_values = edge.g4_live_values;
        else other_edge_live_values += edge.g4_live_values;
    }
    require(edge3_live_values == 3U && other_edge_live_values > 0U,
        "G20 G4 live-through census did not distinguish Edge 3 from the other candidate edges");
}

void test_g20_g7_trials_explain_when_g8_was_not_attempted() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.succeeded() &&
            !result.stats.native_n113_flow_edge_diagnostic_function_reports.empty(),
        "G20 scratch-trial diagnostics were not produced");
    const auto main_function = std::find_if(
        result.stats.native_n113_flow_edge_diagnostic_function_reports.begin(),
        result.stats.native_n113_flow_edge_diagnostic_function_reports.end(),
        [](const auto& item) { return item.function_name == "Main"; });
    require(main_function != result.stats.native_n113_flow_edge_diagnostic_function_reports.end(),
        "G20 Main scratch-trial report was not found");
    const auto& function = *main_function;
    std::uint32_t attempts = 0U;
    std::uint32_t successes = 0U;
    std::uint32_t rejections = 0U;
    std::uint32_t g8_attempted_edges = 0U;
    std::uint32_t g8_not_attempted_edges_with_g4_values = 0U;
    std::uint32_t edges_with_g4_live_values = 0U;
    for (const auto& edge : function.edges) {
        if (edge.g4_live_values > 0U) ++edges_with_g4_live_values;
        require(edge.g7_attempts == edge.g7_trials.size() &&
                edge.g7_schedule_successes + edge.g7_schedule_rejections == edge.g7_attempts,
            "G20 G7 attempt/result counters do not reconcile with per-pair trial records");
        bool any_g7_success = false;
        bool any_g8_completion = false;
        for (const auto& trial : edge.g7_trials) {
            ++attempts;
            if (trial.g7_schedule_succeeded) {
                ++successes;
                any_g7_success = true;
                require(trial.scheduled_operations > 0U && trial.g8_readiness_attempted,
                    "G20 successful G7 trial did not proceed to G8 readiness auditing");
                if (trial.g8_readiness_audit_completed) any_g8_completion = true;
                if (!trial.g8_readiness_audit_completed)
                    require(!trial.g8_rejection_code.empty() && !trial.g8_rejection_message.empty(),
                        "G20 rejected G8 trial has no diagnostic code/message");
            } else {
                ++rejections;
                require(!trial.g7_rejection_code.empty() && !trial.g7_rejection_message.empty() &&
                        !trial.g8_readiness_attempted,
                    "G20 G7 rejection lacks a reason or incorrectly claims G8 was attempted");
            }
        }
        require(edge.g8_readiness_attempted == any_g7_success &&
                edge.g8_readiness_audit_completed == any_g8_completion,
            "G20 edge-level G8 attempted/succeeded flags do not match its G7 trials");
        if (edge.g8_readiness_attempted) {
            ++g8_attempted_edges;
            require(edge.g8_live_values == edge.g4_live_values,
                "G20 G8 live-through count differs from the G4 edge value universe after a G8 attempt");
        } else {
            require(edge.g4_live_values > 0U && edge.g8_live_values == 0U &&
                    edge.g8_endpoint_matches == 0U && edge.g8_endpoint_mismatches == 0U,
                "G20 incorrectly represents a G7-rejected edge as a zero-live G4 edge");
            ++g8_not_attempted_edges_with_g4_values;
        }
    }
    require(attempts == function.g7_attempts && successes == function.g7_schedule_successes &&
            rejections == function.g7_schedule_rejections &&
            g8_attempted_edges == function.g8_readiness_attempts &&
            g8_attempted_edges > 0U &&
            g8_attempted_edges + g8_not_attempted_edges_with_g4_values == edges_with_g4_live_values,
        "G20 function totals do not explain all G7 trials and G8 execution decisions");
}

void test_g20_public_projection_joins_g10_and_g11_without_gate_changes() {
    const auto result = compile_with_n113_g11_edge_eligibility(n113_emitter_pressure_source(), true);
    require(result.succeeded() &&
            result.stats.native_n113_flow_edge_bundle_function_reports.size() ==
                result.stats.native_n113_flow_edge_diagnostic_function_reports.size(),
        "G20 diagnostic and pre-existing G11 public reports do not have matching function inventories");
    const auto g11_main = std::find_if(
        result.stats.native_n113_flow_edge_bundle_function_reports.begin(),
        result.stats.native_n113_flow_edge_bundle_function_reports.end(),
        [](const auto& item) { return item.function_name == "Main"; });
    const auto g20_main = std::find_if(
        result.stats.native_n113_flow_edge_diagnostic_function_reports.begin(),
        result.stats.native_n113_flow_edge_diagnostic_function_reports.end(),
        [](const auto& item) { return item.function_name == "Main"; });
    require(g11_main != result.stats.native_n113_flow_edge_bundle_function_reports.end() &&
            g20_main != result.stats.native_n113_flow_edge_diagnostic_function_reports.end(),
        "G20 Main public projection was not found");
    const auto& g11_function = *g11_main;
    const auto& g20_function = *g20_main;
    require(g11_function.edges.size() == g20_function.edges.size(),
        "G20 diagnostic report changed the existing G11 edge inventory");
    for (std::size_t index = 0U; index < g11_function.edges.size(); ++index) {
        const auto& g11 = g11_function.edges[index];
        const auto& g20 = g20_function.edges[index];
        require(g11.edge_id == g20.edge_id && g11.predecessor_block_id == g20.predecessor_block_id &&
                g11.successor_block_id == g20.successor_block_id,
            "G20 joined G10/G11 evidence by a non-identical edge order or identity");
        require(g20.g10_values_with_evidence == g11.operation_values_with_evidence &&
                g20.g10_values_proven == g11.operation_values_proven &&
                g20.g10_values_mismatched == g11.operation_values_mismatched &&
                g20.g10_values_missing == g11.operation_values_missing &&
                g20.g10_mismatch_counts.register_index == g11.mismatch_counts.register_index &&
                g20.g10_mismatch_counts.flow_register_emitter_memory ==
                    g11.mismatch_counts.flow_register_emitter_memory,
            "G20 G10 value/mismatch evidence does not match the unchanged G11 projection");
        require(g20.g11_exact_schedule == g11.exact_g9_schedule &&
                g20.g11_endpoint_ownership_proven == g11.endpoint_ownership_proven &&
                g20.g11_scratch_liveness_proven == g11.scratch_liveness_proven &&
                g20.g11_operation_operand_ownership_proven == g11.operation_operand_ownership_proven &&
                g20.g11_proof_gate_passed == g11.proof_gate_passed &&
                g20.g11_blockers == g11.blockers,
            "G20 diagnostic projection changed or misreported the existing G11 gate semantics");
        std::size_t operation_records = 0U;
        for (const auto& value : g20.g10_values) operation_records += value.operations.size();
        require(g20.g10_values.size() == g20.g10_values_with_evidence &&
                operation_records > 0U && g20.g6_candidate_values == g11.candidate_values,
            "G20 did not retain per-Value/per-operation G10 evidence for the exact G6 candidate set");
    }
    require(result.stats.native_n113_emitter_actions_inserted == 0U &&
            result.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            result.stats.native_n113_emitter_transfer_bytes_inserted == 0U &&
            result.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "G20 diagnostic export performed insertion or promoted machine readiness");
}

void test_g20_diagnostics_are_deterministic_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g11_edge_eligibility(source, false);
    const auto first = compile_with_n113_g11_edge_eligibility(source, true);
    const auto second = compile_with_n113_g11_edge_eligibility(source, true);
    require(baseline.succeeded() && first.succeeded() && second.succeeded() &&
            baseline.object_bytes == first.object_bytes && first.object_bytes == second.object_bytes,
        "G20 read-only edge diagnostics changed emitted COFF bytes");
    const auto& a_functions = first.stats.native_n113_flow_edge_diagnostic_function_reports;
    const auto& b_functions = second.stats.native_n113_flow_edge_diagnostic_function_reports;
    require(a_functions.size() == b_functions.size() && !a_functions.empty(),
        "G20 deterministic function diagnostic inventory changed between runs");
    for (std::size_t f = 0U; f < a_functions.size(); ++f) {
        const auto& a = a_functions[f];
        const auto& b = b_functions[f];
        require(a.function_index == b.function_index && a.function_name == b.function_name &&
                a.edges.size() == b.edges.size() && a.g7_attempts == b.g7_attempts &&
                a.g7_schedule_successes == b.g7_schedule_successes &&
                a.g7_schedule_rejections == b.g7_schedule_rejections,
            "G20 function diagnostic identity or aggregate trial counts are nondeterministic");
        for (std::size_t e = 0U; e < a.edges.size(); ++e) {
            const auto& x = a.edges[e];
            const auto& y = b.edges[e];
            require(x.edge_id == y.edge_id && x.g4_values.size() == y.g4_values.size() &&
                    x.g6_candidates.size() == y.g6_candidates.size() &&
                    x.g7_trials.size() == y.g7_trials.size() &&
                    x.g6_branch_anchor_identity_exact == y.g6_branch_anchor_identity_exact &&
                    x.g8_readiness_attempted == y.g8_readiness_attempted &&
                    x.g9_schedule_built == y.g9_schedule_built &&
                    x.g11_proof_gate_passed == y.g11_proof_gate_passed,
                "G20 per-edge diagnostic shape or proof state changed between identical runs");
            for (std::size_t t = 0U; t < x.g7_trials.size(); ++t) {
                const auto& p = x.g7_trials[t];
                const auto& q = y.g7_trials[t];
                require(p.gp_temporary_register == q.gp_temporary_register &&
                        p.xmm_temporary_register == q.xmm_temporary_register &&
                        p.g7_schedule_succeeded == q.g7_schedule_succeeded &&
                        p.scheduled_operations == q.scheduled_operations &&
                        p.g7_rejection_code == q.g7_rejection_code &&
                        p.g8_readiness_attempted == q.g8_readiness_attempted &&
                        p.g8_readiness_audit_completed == q.g8_readiness_audit_completed &&
                        p.g8_rejection_code == q.g8_rejection_code,
                    "G20 per-pair scratch trial evidence changed between identical runs");
            }
        }
    }
    require(first.stats.native_n113_emitter_actions_inserted == 0U &&
            first.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            first.stats.native_n113_emitter_transfer_bytes_inserted == 0U &&
            first.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "G20 determinism test observed unexpected machine insertion/readiness");
}


X64CodegenResult compile_with_n113_g21_frame_spill_scratch(const std::string& source,
                                                            bool enabled,
                                                            bool reserve_flow_frame = false) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N113-G21 frame-scratch fixture did not parse");
    X64CodegenOptions options{"n113-g21-frame-spill-scratch", make_coff_x86_64_object_target()};
    options.audit_n113_flow_edge_bundle_eligibility = enabled;
    options.audit_n113_frame_spill_scratch = enabled;
    options.reserve_n113_flow_frame_slots = reserve_flow_frame;
    const auto result = compile_x64_coff_object(parsed.unit, options);
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N113-G21 frame-scratch diagnostic failed: " +
            diagnostic.code + " " + diagnostic.message);
    }
    return result;
}


NativeX64FrameLocationMap g22_memory_cycle_frame_map() {
    NativeX64FrameLocationMap frame;
    frame.spill_slot_offsets.emplace(0U, -8);
    frame.spill_slot_offsets.emplace(1U, -24);
    frame.spill_slot_offsets.emplace(2U, -40);
    return frame;
}

std::vector<NativeRegisterParallelCopy> g22_memory_cycle_copies() {
    return {
        NativeRegisterParallelCopy{1U,
            make_location(NativeRegisterLocationKind::SpillSlot, 0U, 0U),
            make_location(NativeRegisterLocationKind::SpillSlot, 0U, 1U)},
        NativeRegisterParallelCopy{2U,
            make_location(NativeRegisterLocationKind::SpillSlot, 0U, 1U),
            make_location(NativeRegisterLocationKind::SpillSlot, 0U, 0U)}
    };
}

void test_g22_resolver_supports_dedicated_spill_slot_cycle_storage() {
    const auto spill_cycle = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto gp = resolve_native_register_parallel_copies({
        NativeRegisterParallelCopy{1U,
            make_location(NativeRegisterLocationKind::Register, 0U),
            make_location(NativeRegisterLocationKind::Register, 1U)},
        NativeRegisterParallelCopy{2U,
            make_location(NativeRegisterLocationKind::Register, 1U),
            make_location(NativeRegisterLocationKind::Register, 0U)}
    }, spill_cycle);
    require(gp.succeeded && gp.cycle_temporaries_used == 1U && gp.operations.size() == 3U &&
            gp.operations.front().destination.kind == NativeRegisterLocationKind::SpillSlot &&
            gp.operations.front().destination.spill_slot == 2U &&
            gp.operations.front().uses_cycle_temporary,
        "G22 GP cycle was not saved to its dedicated SpillSlot");
    std::uint32_t cycle_reads = 0U;
    for (const auto& operation : gp.operations) {
        if (operation.source.kind == NativeRegisterLocationKind::SpillSlot &&
            operation.source.spill_slot == 2U) {
            ++cycle_reads;
            require(operation.uses_cycle_temporary,
                "G22 SpillSlot cycle read lost its cycle-temporary marker");
        }
    }
    require(cycle_reads == 1U,
        "G22 GP resolved schedule did not read the saved cycle value exactly once");

    const auto xmm = resolve_native_register_parallel_copies({
        NativeRegisterParallelCopy{3U,
            make_location(NativeRegisterLocationKind::Register, 0U),
            make_location(NativeRegisterLocationKind::Register, 1U)},
        NativeRegisterParallelCopy{4U,
            make_location(NativeRegisterLocationKind::Register, 1U),
            make_location(NativeRegisterLocationKind::Register, 0U)}
    }, spill_cycle);
    require(xmm.succeeded && xmm.cycle_temporaries_used == 1U &&
            xmm.operations.size() == 3U &&
            xmm.operations.front().destination.spill_slot == 2U,
        "G22 XMM cycle did not use the same storage-level SpillSlot contract");

    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values = {
        NativeRegisterValueInfo{1U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{2U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{3U, NativeRegisterClass::GeneralPurpose},
        NativeRegisterValueInfo{4U, NativeRegisterClass::GeneralPurpose}};
    function.blocks = {
        NativeRegisterBasicBlock{0U, {}, {1U, 2U}, {}},
        NativeRegisterBasicBlock{1U, {}, {3U}, {0U}},
        NativeRegisterBasicBlock{2U, {}, {3U}, {0U}},
        NativeRegisterBasicBlock{3U, {}, {}, {1U, 2U}}};
    const std::vector<NativeRegisterPhiNode> phis{
        NativeRegisterPhiNode{3U, 3U, {{1U, 1U}, {2U, 2U}}},
        NativeRegisterPhiNode{3U, 4U, {{1U, 2U}, {2U, 1U}}}};
    const std::map<std::uint32_t, NativeRegisterLocation> locations{
        {1U, make_location(NativeRegisterLocationKind::Register, 0U)},
        {2U, make_location(NativeRegisterLocationKind::Register, 1U)},
        {3U, make_location(NativeRegisterLocationKind::Register, 1U)},
        {4U, make_location(NativeRegisterLocationKind::Register, 0U)}};
    const auto lowered = lower_native_register_phi_edges(function, phis, locations,
        make_location(NativeRegisterLocationKind::SpillSlot, 0U, 9U));
    const auto cyclic_edge = std::find_if(lowered.edge_transfers.begin(), lowered.edge_transfers.end(),
        [](const NativeRegisterPhiEdgeTransfer& edge) {
            return edge.predecessor_block_id == 1U && edge.successor_block_id == 3U;
        });
    const auto phi_cycle_operations = cyclic_edge == lowered.edge_transfers.end()
        ? std::vector<NativeRegisterParallelCopyOperation>{} : cyclic_edge->operations;
    const auto phi_cycle_marker_count = static_cast<std::size_t>(std::count_if(
        phi_cycle_operations.begin(), phi_cycle_operations.end(),
        [](const NativeRegisterParallelCopyOperation& operation) {
            return operation.uses_cycle_temporary;
        }));
    require(lowered.succeeded && lowered.edge_transfers.size() == 2U &&
            cyclic_edge != lowered.edge_transfers.end() && phi_cycle_operations.size() == 3U &&
            phi_cycle_marker_count == 2U &&
            std::count_if(phi_cycle_operations.begin(), phi_cycle_operations.end(),
                [](const NativeRegisterParallelCopyOperation& operation) {
                    return operation.destination.kind == NativeRegisterLocationKind::SpillSlot &&
                           operation.destination.spill_slot == 9U;
                }) == 1 &&
            std::count_if(phi_cycle_operations.begin(), phi_cycle_operations.end(),
                [](const NativeRegisterParallelCopyOperation& operation) {
                    return operation.source.kind == NativeRegisterLocationKind::SpillSlot &&
                           operation.source.spill_slot == 9U;
                }) == 1,
        "G22 Phi-edge lowering did not emit one SpillSlot cycle save and one matching restore");
}

void test_g22_resolver_rejects_home_and_aliased_cycle_storage() {
    const auto copies = std::vector<NativeRegisterParallelCopy>{
        NativeRegisterParallelCopy{1U,
            make_location(NativeRegisterLocationKind::Register, 0U),
            make_location(NativeRegisterLocationKind::Register, 1U)},
        NativeRegisterParallelCopy{2U,
            make_location(NativeRegisterLocationKind::Register, 1U),
            make_location(NativeRegisterLocationKind::Register, 0U)}
    };
    const auto home = resolve_native_register_parallel_copies(copies,
        make_location(NativeRegisterLocationKind::HomeSlot, 0U, 7U));
    require(!home.succeeded && !home.diagnostics.empty() &&
            home.diagnostics.front().code == "N80E0001",
        "G22 incorrectly accepted a canonical HomeSlot as cycle scratch storage");
    const auto alias = resolve_native_register_parallel_copies({
        NativeRegisterParallelCopy{3U,
            make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U),
            make_location(NativeRegisterLocationKind::Register, 1U)},
        NativeRegisterParallelCopy{4U,
            make_location(NativeRegisterLocationKind::Register, 1U),
            make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U)}
    }, make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U));
    require(!alias.succeeded && !alias.diagnostics.empty() &&
            alias.diagnostics.front().code == "N80E0004",
        "G22 accepted a cycle SpillSlot that aliases an input source or destination");
}

void test_g22_gp_memory_cycle_encoding_uses_separate_expansion_register() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto scheduled = resolve_native_register_parallel_copies(g22_memory_cycle_copies(), cycle_slot);
    require(scheduled.succeeded && scheduled.cycle_temporaries_used == 1U &&
            scheduled.operations.size() == 3U,
        "G22 GP memory-to-memory cycle did not produce a three-operation schedule");
    const auto encoded = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        scheduled.operations,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, make_location(NativeRegisterLocationKind::Temporary, 11U),
        g22_memory_cycle_frame_map());
    const std::vector<std::uint8_t> expected{
        0x4CU, 0x8BU, 0x9DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0x4CU, 0x89U, 0x9DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
        0x4CU, 0x8BU, 0x9DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU,
        0x4CU, 0x89U, 0x9DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0x4CU, 0x8BU, 0x9DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
        0x4CU, 0x89U, 0x9DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU};
    require(encoded.succeeded && encoded.bytes == expected &&
            encoded.machine_moves_emitted == 6U &&
            encoded.memory_to_memory_expansions == 3U &&
            encoded.temporary_register_moves == 6U,
        "G22 GP memory cycle was not expanded through the separate R11 scratch register");
}

void test_g22_xmm_memory_cycle_encoding_uses_separate_expansion_register() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto scheduled = resolve_native_register_parallel_copies(g22_memory_cycle_copies(), cycle_slot);
    const auto encoded = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        scheduled.operations,
        {{1U, NativeRegisterClass::FloatingPoint}, {2U, NativeRegisterClass::FloatingPoint}},
        cycle_slot, make_location(NativeRegisterLocationKind::Temporary, 11U),
        g22_memory_cycle_frame_map());
    const std::vector<std::uint8_t> expected{
        0xF3U, 0x44U, 0x0FU, 0x7EU, 0x9DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x44U, 0x0FU, 0xD6U, 0x9DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
        0xF3U, 0x44U, 0x0FU, 0x7EU, 0x9DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x44U, 0x0FU, 0xD6U, 0x9DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0xF3U, 0x44U, 0x0FU, 0x7EU, 0x9DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x44U, 0x0FU, 0xD6U, 0x9DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU};
    require(scheduled.succeeded && encoded.succeeded && encoded.bytes == expected &&
            encoded.machine_moves_emitted == 6U &&
            encoded.memory_to_memory_expansions == 3U &&
            encoded.temporary_register_moves == 6U,
        "G22 XMM memory cycle was not expanded through the separate XMM11 scratch register");
}

void test_g22_encoder_rejects_expansion_register_and_frame_slot_aliases() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto scheduled = resolve_native_register_parallel_copies(g22_memory_cycle_copies(), cycle_slot);
    auto operations = scheduled.operations;
    operations.push_back(NativeRegisterParallelCopyOperation{
        3U, make_location(NativeRegisterLocationKind::Register, 11U),
        make_location(NativeRegisterLocationKind::Register, 12U), false});
    auto frame = g22_memory_cycle_frame_map();
    const auto register_alias = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        operations,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose},
         {3U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, make_location(NativeRegisterLocationKind::Temporary, 11U), frame);
    require(!register_alias.succeeded && !register_alias.diagnostics.empty() &&
            register_alias.diagnostics.front().code == "N113G22E0006",
        "G22 allowed the GP expansion register to alias a live transfer operand");

    frame.home_slot_offsets.emplace(0U, -40);
    const auto frame_alias = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        scheduled.operations,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, make_location(NativeRegisterLocationKind::Temporary, 11U), frame);
    require(!frame_alias.succeeded && !frame_alias.diagnostics.empty() &&
            frame_alias.diagnostics.front().code == "N113G22E0005",
        "G22 accepted a cycle SpillSlot overlapping a canonical HomeSlot displacement");

    auto marker_mismatch = scheduled.operations;
    marker_mismatch.front().uses_cycle_temporary = false;
    const auto marker_rejected = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        marker_mismatch,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, make_location(NativeRegisterLocationKind::Temporary, 11U),
        g22_memory_cycle_frame_map());
    require(!marker_rejected.succeeded && !marker_rejected.diagnostics.empty() &&
            marker_rejected.diagnostics.front().code == "N113G22E0011",
        "G22 accepted a schedule whose cycle-temporary marker disagrees with its SpillSlot access");
}

void test_g24_gp_memory_cycle_uses_xmm_movq_without_cross_bank_alias_false_positive() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto scheduled = resolve_native_register_parallel_copies(g22_memory_cycle_copies(), cycle_slot);
    require(scheduled.succeeded && scheduled.operations.size() == 3U,
        "G24 GP spill cycle did not resolve through its dedicated memory cycle slot");

    auto frame = g22_memory_cycle_frame_map();
    frame.spill_slot_offsets.emplace(3U, -48);
    auto operations = scheduled.operations;
    // GP register index 1 is RCX; XMM1 is a physically distinct register.
    // Equal numeric indices across the two register banks must not alias.
    operations.push_back(NativeRegisterParallelCopyOperation{
        3U, make_location(NativeRegisterLocationKind::Register, 1U),
        make_location(NativeRegisterLocationKind::SpillSlot, 0U, 3U), false});

    const auto encoded = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
        operations,
        {{1U, NativeRegisterClass::GeneralPurpose},
         {2U, NativeRegisterClass::GeneralPurpose},
         {3U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, make_location(NativeRegisterLocationKind::Temporary, 1U), frame);

    const std::vector<std::uint8_t> expected{
        0xF3U, 0x0FU, 0x7EU, 0x8DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x0FU, 0xD6U, 0x8DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
        0xF3U, 0x0FU, 0x7EU, 0x8DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x0FU, 0xD6U, 0x8DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
        0xF3U, 0x0FU, 0x7EU, 0x8DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
        0x66U, 0x0FU, 0xD6U, 0x8DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU,
        0x48U, 0x89U, 0x8DU, 0xD0U, 0xFFU, 0xFFU, 0xFFU};
    require(encoded.succeeded && encoded.bytes == expected &&
            encoded.machine_moves_emitted == 7U &&
            encoded.memory_to_memory_expansions == 3U &&
            encoded.temporary_register_moves == 6U,
        "G24 GP memory cycle did not use bit-preserving MOVQ through XMM1 or incorrectly aliased RCX with XMM1");

    for (std::uint8_t xmm_index = 1U; xmm_index <= 5U; ++xmm_index) {
        const auto xmm_encoded = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
            operations,
            {{1U, NativeRegisterClass::GeneralPurpose},
             {2U, NativeRegisterClass::GeneralPurpose},
             {3U, NativeRegisterClass::GeneralPurpose}},
            cycle_slot, make_location(NativeRegisterLocationKind::Temporary, xmm_index), frame);
        auto expected_xmm = expected;
        const auto modrm = static_cast<std::uint8_t>(0x85U | (xmm_index << 3U));
        for (const auto index : {3U, 11U, 19U, 27U, 35U, 43U}) expected_xmm[index] = modrm;
        require(xmm_encoded.succeeded && xmm_encoded.bytes == expected_xmm &&
                xmm_encoded.machine_moves_emitted == 7U && xmm_encoded.memory_to_memory_expansions == 3U,
            "G24 failed exact byte encoding for a register in the Win64 volatile XMM1-XMM5 expansion pool");
    }
}

void test_g24_gp_movq_expansion_rejects_fixed_and_nonvolatile_xmm_registers() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto scheduled = resolve_native_register_parallel_copies(g22_memory_cycle_copies(), cycle_slot);
    const auto frame = g22_memory_cycle_frame_map();
    const std::map<std::uint32_t, NativeRegisterClass> classes{
        {1U, NativeRegisterClass::GeneralPurpose},
        {2U, NativeRegisterClass::GeneralPurpose}};

    for (const auto index : {0U, 6U, 15U}) {
        const auto rejected = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
            scheduled.operations, classes, cycle_slot,
            make_location(NativeRegisterLocationKind::Temporary,
                          static_cast<std::uint8_t>(index)), frame);
        require(!rejected.succeeded && !rejected.diagnostics.empty() &&
                rejected.diagnostics.front().code == "N113G24E0001",
            "G24 accepted XMM0 or a Win64 nonvolatile XMM6-XMM15 expansion register");
    }
}

void test_g24_gp_movq_expansion_rejects_wrong_class_and_untyped_temporary() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto scheduled = resolve_native_register_parallel_copies(g22_memory_cycle_copies(), cycle_slot);
    const auto frame = g22_memory_cycle_frame_map();

    const auto wrong_class = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
        scheduled.operations,
        {{1U, NativeRegisterClass::FloatingPoint}, {2U, NativeRegisterClass::FloatingPoint}},
        cycle_slot, make_location(NativeRegisterLocationKind::Temporary, 1U), frame);
    require(!wrong_class.succeeded && !wrong_class.diagnostics.empty() &&
            wrong_class.diagnostics.front().code == "N113G24E0002",
        "G24 accepted XMM expansion for a non-GP transfer schedule");

    const auto untyped = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
        scheduled.operations,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, make_location(NativeRegisterLocationKind::Register, 1U), frame);
    require(!untyped.succeeded && !untyped.diagnostics.empty() &&
            untyped.diagnostics.front().code == "N113G24E0003",
        "G24 accepted a physical Register instead of an explicit expansion Temporary");
}

struct G25GpXmmFixture {
    G4FlowEdgeFixture fixture;
    NativeX64FlowEdgeLocationAuditPlan g4;
    NativeX64FlowEdgeTransferCandidatePlan g6;
};

G25GpXmmFixture make_g25_gp_xmm_fixture(bool add_live_fp_value = true) {
    G25GpXmmFixture artifacts;
    artifacts.fixture = make_g4_flow_edge_fixture();
    if (add_live_fp_value) {
        artifacts.fixture.function.register_ir.values.push_back(
            NativeRegisterValueInfo{8U, NativeRegisterClass::FloatingPoint});

        // Keep one Flow definition per instruction, matching the established
        // G8 live-scratch-occupant fixture. Appending Value 8 to the existing
        // Value 7 definition made this synthetic IR invalid for N112 analysis.
        auto& entry = artifacts.fixture.function.register_ir.blocks[0U];
        auto fp_declare = flow_instruction(NativeRegisterInstructionOriginKind::FlowDeclare, 1U);
        fp_declare.flow_block_id = 0U;
        fp_declare.definitions = {8U};
        entry.instructions.push_back(fp_declare);

        // Block 2 already has operation index 2 / Flow ID (3,3); the next
        // operation is index 3 / Flow ID (3,4). Keep both identities aligned.
        auto fp_read = flow_instruction(NativeRegisterInstructionOriginKind::FlowRead, 3U);
        fp_read.flow_block_id = 2U;
        fp_read.flow_operation_id = (std::uint64_t{3U} << 32U) | 4U;
        fp_read.definitions.clear();
        fp_read.uses = {8U};
        artifacts.fixture.function.register_ir.blocks[2U].instructions.push_back(fp_read);
    }
    artifacts.fixture.analysis = build_fixture(artifacts.fixture.function.register_ir);
    if (!(artifacts.fixture.analysis.liveness.succeeded &&
          artifacts.fixture.analysis.liveness.converged &&
          artifacts.fixture.analysis.intervals.succeeded &&
          artifacts.fixture.analysis.segments.succeeded &&
          artifacts.fixture.analysis.allocation.succeeded)) {
        std::string detail = "G25 fixture could not build converged CFG liveness and N112 analysis";
        const auto append = [&detail](const char* stage,
                                      const std::vector<NativeRegisterIRDiagnostic>& diagnostics) {
            if (!diagnostics.empty()) {
                detail += std::string(": ") + stage + " " + diagnostics.front().code +
                    " " + diagnostics.front().message;
            }
        };
        append("liveness", artifacts.fixture.analysis.liveness.diagnostics);
        append("intervals", artifacts.fixture.analysis.intervals.diagnostics);
        append("segments", artifacts.fixture.analysis.segments.diagnostics);
        append("allocation", artifacts.fixture.analysis.allocation.diagnostics);
        throw std::runtime_error(detail);
    }
    artifacts.fixture.flow_segments = build_plan(artifacts.fixture.analysis);
    require(artifacts.fixture.flow_segments.succeeded,
        "G25 fixture could not build N112 Flow segment plan");

    artifacts.fixture.flow_frame.spill_slot_offsets = {{0U, -8}};
    artifacts.fixture.flow_frame.home_slot_offsets = {{0U, -16}};
    for (auto& segment : artifacts.fixture.flow_segments.segments) {
        if (segment.segment.value_id == 7U) {
            if (segment.segment.block_id == 0U) {
                segment.location = make_location(NativeRegisterLocationKind::Register, 3U);
            } else if (segment.segment.block_id == 1U) {
                segment.location = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 0U);
            } else {
                segment.location = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 0U);
            }
        } else if (segment.segment.value_id == 8U) {
            segment.location = make_location(NativeRegisterLocationKind::Register, 3U);
        }
    }
    if (add_live_fp_value) {
        NativeX64MachineValueLocation fp_value;
        fp_value.value_id = 8U;
        fp_value.register_class = NativeRegisterClass::FloatingPoint;
        fp_value.active_location = make_location(NativeRegisterLocationKind::Register, 3U);
        fp_value.canonical_home = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 2U);
        artifacts.fixture.emitter_locations.values.emplace(8U, fp_value);
    }
    artifacts.g4 = audit_g4_fixture(artifacts.fixture);
    require(artifacts.g4.succeeded,
        "G25 fixture did not produce exact G4 live-through endpoint evidence");
    const auto branch_anchors = make_g6_branch_anchors(artifacts.g4);
    std::map<std::uint32_t, NativeRegisterClass> classes;
    for (const auto& value : artifacts.fixture.function.register_ir.values)
        classes.emplace(value.value_id, value.register_class);
    artifacts.g6 = plan_native_x64_flow_edge_transfer_candidates(
        artifacts.g4, branch_anchors, classes);
    require(artifacts.g6.succeeded && !artifacts.g6.ready_for_machine_insertion,
        "G25 fixture did not produce a validated non-insertion-ready G6 inventory");
    return artifacts;
}

void test_g25_reconciles_g4_live_sets_and_selects_first_dead_volatile_xmm() {
    const auto fixture = make_g25_gp_xmm_fixture(true);
    const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {1U, 2U});
    const auto edge = std::find_if(plan.edges.begin(), plan.edges.end(),
        [](const auto& item) { return item.edge_id == 3U; });
    require(plan.succeeded && plan.g4_live_sets_reconciled &&
            plan.candidate_edge_identities_exact && plan.cfg_edges_reconciled == 3U &&
            edge != plan.edges.end() && edge->g4_live_set_complete &&
            edge->gp_only_candidates && edge->gp_memory_to_memory_required &&
            edge->xmm_expansion_selection_proven && edge->selected_xmm_register == 4U &&
            edge->xmm_candidates.size() == 5U &&
            edge->xmm_candidates[0U].expression_reserved &&
            edge->xmm_candidates[1U].expression_reserved &&
            edge->xmm_candidates[2U].live_value_conflict &&
            edge->xmm_candidates[2U].conflicting_value_ids == std::vector<std::uint32_t>{8U} &&
            edge->xmm_candidates[3U].available && edge->xmm_candidates[4U].available &&
            !plan.machine_insertion_ready,
        "G25 failed to reconcile exact G4 values or select XMM4 after reserved/live conflicts");
}

void test_g25_gp_numeric_register_index_does_not_alias_same_xmm_index() {
    const auto fixture = make_g25_gp_xmm_fixture(false);
    const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {});
    const auto edge = std::find_if(plan.edges.begin(), plan.edges.end(),
        [](const auto& item) { return item.edge_id == 3U; });
    require(plan.succeeded && edge != plan.edges.end() && edge->gp_only_candidates &&
            edge->gp_memory_to_memory_required && edge->xmm_expansion_selection_proven &&
            edge->selected_xmm_register == 1U &&
            std::all_of(edge->xmm_candidates.begin(), edge->xmm_candidates.end(),
                [](const auto& candidate) { return candidate.available; }),
        "G25 incorrectly treated a live GP register number as an XMM bank alias");
}

void test_g25_rejects_missing_g4_live_endpoint_record() {
    auto fixture = make_g25_gp_xmm_fixture(true);
    const auto found = std::find_if(fixture.g4.values.begin(), fixture.g4.values.end(),
        [](const auto& value) { return value.edge_id == 3U && value.value_id == 8U; });
    require(found != fixture.g4.values.end(), "G25 negative fixture has no FP G4 live-value record");
    fixture.g4.values.erase(found);
    fixture.g4.live_values_audited--;
    const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {});
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G25E000B",
        "G25 accepted a G4 table missing a Value in live_out intersect live_in");
}

void test_g25_rejects_duplicate_g4_value_endpoint_record() {
    auto fixture = make_g25_gp_xmm_fixture(true);
    const auto found = std::find_if(fixture.g4.values.begin(), fixture.g4.values.end(),
        [](const auto& value) { return value.edge_id == 3U && value.value_id == 8U; });
    require(found != fixture.g4.values.end(), "G25 duplicate fixture has no FP G4 live-value record");
    fixture.g4.values.push_back(*found);
    fixture.g4.live_values_audited++;
    const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {});
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G25E0007",
        "G25 accepted duplicate exact-edge Value endpoint evidence");
}

void test_g25_rejects_g6_candidate_that_disagrees_with_g4_endpoint() {
    auto fixture = make_g25_gp_xmm_fixture(true);
    require(!fixture.g6.candidates.empty(), "G25 candidate mismatch fixture has no G6 candidate");
    fixture.g6.candidates.front().copy.destination =
        make_location(NativeRegisterLocationKind::Register, 11U);
    const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {});
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G25E000F",
        "G25 accepted a G6 endpoint that differs from its G4/N112 evidence");
}

void test_g25_is_deterministic_and_never_promotes_insertion_readiness() {
    const auto fixture = make_g25_gp_xmm_fixture(true);
    const auto first = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {1U, 2U});
    const auto second = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {1U, 2U});
    require(first.succeeded && second.succeeded && !first.machine_insertion_ready &&
            !second.machine_insertion_ready && first.edges.size() == second.edges.size() &&
            first.xmm_register_candidates_dead == second.xmm_register_candidates_dead &&
            first.edges.front().edge_id == second.edges.front().edge_id &&
            first.edges.back().selected_xmm_register == second.edges.back().selected_xmm_register,
        "G25 edge/XMM liveness proof is nondeterministic or incorrectly marks insertion ready");
}

void test_g25_rejects_g4_live_value_counter_mismatch() {
    auto fixture = make_g25_gp_xmm_fixture(true);
    fixture.g4.live_values_audited--;
    const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {1U, 2U});
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G25E0016",
        "G25 accepted a G4 live-value count that does not match its actual evidence records");
}

void test_g25_rejects_entirely_omitted_g6_candidate_edge() {
    auto fixture = make_g25_gp_xmm_fixture(true);
    fixture.g6.candidates.pop_back();
    fixture.g6.candidate_copies = static_cast<std::uint32_t>(fixture.g6.candidates.size());
    std::set<FlowEdgeId> edge_ids;
    for (const auto& candidate : fixture.g6.candidates) edge_ids.insert(candidate.edge_id);
    fixture.g6.candidate_edges = static_cast<std::uint32_t>(edge_ids.size());
    const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {1U, 2U});
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G25E0018",
        "G25 accepted a G6 inventory with an entirely omitted G4 location-change candidate");
}

NativeX64GpXmmExpansionLivenessPlan make_g26_two_edge_g25_proof() {
    const auto fixture = make_g25_gp_xmm_fixture(true);
    auto proof = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        fixture.fixture.function, fixture.fixture.analysis.liveness,
        fixture.g4, fixture.g6, {1U, 2U});
    require(proof.succeeded && proof.all_required_edges_have_dead_xmm &&
            proof.gp_memory_expansion_edges == 1U,
        "G26 could not obtain its base exact-edge G25 XMM proof");
    const auto original = std::find_if(proof.edges.begin(), proof.edges.end(),
        [](const auto& edge) { return edge.gp_memory_to_memory_required; });
    require(original != proof.edges.end(), "G26 base G25 proof has no GP memory-expansion edge");
    auto second = *original;
    second.edge_id = 4U;
    proof.edges.push_back(second);
    proof.candidate_edges_audited = static_cast<std::uint32_t>(proof.edges.size());
    proof.gp_memory_expansion_edges = 2U;
    proof.edges_with_dead_expansion_register = 2U;
    proof.all_required_edges_have_dead_xmm = true;
    return proof;
}

std::vector<NativeRegisterParallelCopyOperation> make_g26_cycle_operations(
    const NativeRegisterLocation cycle_slot) {
    const auto resolved = resolve_native_register_parallel_copies({
        NativeRegisterParallelCopy{1U,
            make_location(NativeRegisterLocationKind::Register, 8U),
            make_location(NativeRegisterLocationKind::Register, 9U)},
        NativeRegisterParallelCopy{2U,
            make_location(NativeRegisterLocationKind::Register, 9U),
            make_location(NativeRegisterLocationKind::Register, 8U)}
    }, cycle_slot);
    require(resolved.succeeded && resolved.cycle_temporaries_used == 1U &&
            resolved.operations.size() == 3U,
        "G26 fixture could not resolve its baseline one-cycle GP schedule");
    return resolved.operations;
}

void test_g26_reconciles_multiple_edges_and_reuses_edge_local_cycle_storage() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    auto frame = g22_memory_cycle_frame_map();
    const auto operations = make_g26_cycle_operations(cycle_slot);
    const std::vector<NativeX64GpMemoryCycleEdgeScheduleInput> schedules{
        NativeX64GpMemoryCycleEdgeScheduleInput{3U, operations},
        NativeX64GpMemoryCycleEdgeScheduleInput{4U, operations}};
    const auto plan = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        make_g26_two_edge_g25_proof(), schedules, cycle_slot, frame);
    require(plan.succeeded && plan.g25_edge_coverage_exact &&
            plan.ordered_cycle_protocol_valid && plan.cycle_slot_frame_mapping_valid &&
            plan.edges_audited == 2U && plan.total_cycle_transactions_audited == 2U &&
            plan.cycle_slot_reused_across_edges && plan.distinct_xmm_registers_selected == 1U &&
            plan.edges[0U].selected_xmm_register == 4U &&
            plan.edges[1U].selected_xmm_register == 4U && !plan.machine_insertion_ready,
        "G26 did not reconcile two exact edges with safe edge-local reuse of one cycle slot/XMM register");
}

void test_g26_accepts_two_sequential_cycles_on_one_edge() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto one_cycle = make_g26_cycle_operations(cycle_slot);
    auto two_cycles = one_cycle;
    auto second = one_cycle;
    for (auto& operation : second) operation.value_id += 10U;
    two_cycles.insert(two_cycles.end(), second.begin(), second.end());
    const auto proof_fixture = make_g25_gp_xmm_fixture(true);
    const auto proof = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        proof_fixture.fixture.function, proof_fixture.fixture.analysis.liveness,
        proof_fixture.g4, proof_fixture.g6, {1U, 2U});
    const std::vector<NativeX64GpMemoryCycleEdgeScheduleInput> schedules{
        NativeX64GpMemoryCycleEdgeScheduleInput{3U, two_cycles}};
    const auto plan = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, schedules, cycle_slot, g22_memory_cycle_frame_map());
    require(plan.succeeded && plan.edges_audited == 1U &&
            plan.total_cycle_transactions_audited == 2U &&
            plan.cycle_slot_reused_across_transactions &&
            plan.edges.front().transactions[0U].restore_operation_index <
                plan.edges.front().transactions[1U].save_operation_index,
        "G26 failed to recognize two strictly sequential cycles on one edge");
}

void test_g26_rejects_cycle_restore_before_save() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    auto operations = make_g26_cycle_operations(cycle_slot);
    operations.front().source = cycle_slot;
    operations.front().destination = make_location(NativeRegisterLocationKind::Register, 8U);
    operations.front().uses_cycle_temporary = true;
    const auto proof_fixture = make_g25_gp_xmm_fixture(true);
    const auto proof = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        proof_fixture.fixture.function, proof_fixture.fixture.analysis.liveness,
        proof_fixture.g4, proof_fixture.g6, {1U, 2U});
    const auto plan = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}}, cycle_slot, g22_memory_cycle_frame_map());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G26E0010",
        "G26 accepted a cycle restore before any save");
}

void test_g26_rejects_cycle_slot_overwrite_before_restore() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    auto operations = make_g26_cycle_operations(cycle_slot);
    operations.insert(operations.begin() + 1, operations.front());
    const auto proof_fixture = make_g25_gp_xmm_fixture(true);
    const auto proof = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        proof_fixture.fixture.function, proof_fixture.fixture.analysis.liveness,
        proof_fixture.g4, proof_fixture.g6, {1U, 2U});
    const auto plan = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}}, cycle_slot, g22_memory_cycle_frame_map());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G26E000F",
        "G26 accepted a second save that overwrites a still-live cycle value");
}

void test_g26_rejects_cycle_restore_value_identity_mismatch() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    auto operations = make_g26_cycle_operations(cycle_slot);
    const auto restore = std::find_if(operations.begin(), operations.end(),
        [&](const auto& op) { return op.source.kind == NativeRegisterLocationKind::SpillSlot &&
            op.source.spill_slot == cycle_slot.spill_slot; });
    require(restore != operations.end(), "G26 negative fixture has no cycle restore operation");
    restore->value_id += 100U;
    const auto proof_fixture = make_g25_gp_xmm_fixture(true);
    const auto proof = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        proof_fixture.fixture.function, proof_fixture.fixture.analysis.liveness,
        proof_fixture.g4, proof_fixture.g6, {1U, 2U});
    const auto plan = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}}, cycle_slot, g22_memory_cycle_frame_map());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G26E0011",
        "G26 accepted a restore of a different ValueId from the saved cycle value");
}

void test_g26_rejects_cycle_marker_without_spill_slot_access() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    auto operations = make_g26_cycle_operations(cycle_slot);
    auto ordinary = NativeRegisterParallelCopyOperation{
        20U, make_location(NativeRegisterLocationKind::Register, 4U),
        make_location(NativeRegisterLocationKind::Register, 5U), true};
    operations.insert(operations.begin() + 1, ordinary);
    const auto proof_fixture = make_g25_gp_xmm_fixture(true);
    const auto proof = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        proof_fixture.fixture.function, proof_fixture.fixture.analysis.liveness,
        proof_fixture.g4, proof_fixture.g6, {1U, 2U});
    const auto plan = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}}, cycle_slot, g22_memory_cycle_frame_map());
    require(!plan.succeeded && !plan.diagnostics.empty() &&
            plan.diagnostics.front().code == "N113G26E000C",
        "G26 accepted a cycle marker on an operation that does not touch the SpillSlot");
}

void test_g26_rejects_missing_or_duplicate_exact_edge_schedule() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto operations = make_g26_cycle_operations(cycle_slot);
    const auto proof = make_g26_two_edge_g25_proof();
    const auto missing = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}}, cycle_slot, g22_memory_cycle_frame_map());
    require(!missing.succeeded && !missing.diagnostics.empty() &&
            missing.diagnostics.front().code == "N113G26E0008",
        "G26 accepted an omitted whole-edge schedule");
    const auto duplicate = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}, {3U, operations}}, cycle_slot, g22_memory_cycle_frame_map());
    require(!duplicate.succeeded && !duplicate.diagnostics.empty() &&
            duplicate.diagnostics.front().code == "N113G26E0009",
        "G26 accepted duplicate schedules for one exact EdgeId");
}

void test_g26_rejects_unmapped_or_physically_aliased_cycle_slot() {
    const auto cycle_slot = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const auto operations = make_g26_cycle_operations(cycle_slot);
    const auto proof_fixture = make_g25_gp_xmm_fixture(true);
    const auto proof = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
        proof_fixture.fixture.function, proof_fixture.fixture.analysis.liveness,
        proof_fixture.g4, proof_fixture.g6, {1U, 2U});
    auto unmapped = g22_memory_cycle_frame_map();
    unmapped.spill_slot_offsets.erase(2U);
    const auto missing = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}}, cycle_slot, unmapped);
    require(!missing.succeeded && !missing.diagnostics.empty() &&
            missing.diagnostics.front().code == "N113G26E0003",
        "G26 accepted an unmapped cycle SpillSlot");
    auto alias = g22_memory_cycle_frame_map();
    alias.home_slot_offsets.emplace(9U, -40);
    const auto aliased = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
        proof, {{3U, operations}}, cycle_slot, alias);
    require(!aliased.succeeded && !aliased.diagnostics.empty() &&
            aliased.diagnostics.front().code == "N113G26E0005",
        "G26 accepted a cycle SpillSlot that physically aliases a HomeSlot");
}

void test_g21_inventory_separates_active_emitter_and_hypothetical_flow_slots() {
    const auto result = compile_with_n113_g21_frame_spill_scratch(n113_emitter_pressure_source(), true);
    require(result.succeeded() &&
            result.stats.native_n113_frame_scratch_diagnostic_function_reports.size() == 4U,
        "G21 did not emit one frame-scratch inventory per FunctionEmitter function");
    const auto main = std::find_if(
        result.stats.native_n113_frame_scratch_diagnostic_function_reports.begin(),
        result.stats.native_n113_frame_scratch_diagnostic_function_reports.end(),
        [](const auto& item) { return item.function_name == "Main"; });
    require(main != result.stats.native_n113_frame_scratch_diagnostic_function_reports.end(),
        "G21 Main frame-scratch inventory was not found");
    require(main->active_frame_plan_succeeded && main->active_frame_size_bytes > 0U &&
            !main->flow_frame_slots_reserved_in_active_frame &&
            main->hypothetical_flow_spill_slots_mapped == main->flow_spill_slots_planned,
        "G21 conflated the actual active frame with the hypothetical N112 Flow frame map");
    require(main->edges.size() == 6U,
        "G21 frame-scratch report does not cover all six exact G11 candidate edges");
    for (const auto& edge : main->edges) {
        require(edge.flow_spill_slots.size() == main->flow_spill_slots_planned,
            "G21 did not inventory every N112 Flow SpillSlot on every exact edge");
        for (const auto& slot : edge.flow_spill_slots) {
            require(slot.frame_mapping_available && slot.hypothetical_frame_displacement < 0 &&
                    (slot.hypothetical_frame_displacement % 8) == 0 &&
                    !slot.active_frame_reserved && !slot.usable_in_current_active_frame &&
                    !slot.current_g7_accepts_spill_slot_temporary,
                "G21 misreported a hypothetical Flow SpillSlot as active or G7-compatible");
        }
    }
    require(result.stats.native_n113_emitter_actions_inserted == 0U &&
            result.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            result.stats.native_n113_emitter_transfer_bytes_inserted == 0U &&
            result.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "G21 frame inventory changed machine code or insertion readiness");
}

void test_g21_flow_spill_candidate_requires_no_g4_live_endpoint_alias() {
    const auto result = compile_with_n113_g21_frame_spill_scratch(n113_emitter_pressure_source(), true);
    const auto main = std::find_if(
        result.stats.native_n113_frame_scratch_diagnostic_function_reports.begin(),
        result.stats.native_n113_frame_scratch_diagnostic_function_reports.end(),
        [](const auto& item) { return item.function_name == "Main"; });
    require(main != result.stats.native_n113_frame_scratch_diagnostic_function_reports.end(),
        "G21 Main edge inventory was not found");
    std::uint32_t live_values = 0U;
    bool saw_edge_with_live_value = false;
    for (const auto& edge : main->edges) {
        live_values += edge.g4_live_values;
        saw_edge_with_live_value = saw_edge_with_live_value || edge.g4_live_values != 0U;
        for (const auto& slot : edge.flow_spill_slots) {
            require(slot.live_endpoint_uses == 0U || !slot.no_g4_live_endpoint_alias,
                "G21 reported a SpillSlot as endpoint-alias-free despite an exact G4 use");
            require(slot.candidate_after_flow_frame_reservation ==
                    (slot.frame_mapping_available && slot.live_endpoint_uses == 0U),
                "G21 future candidate predicate does not reconcile frame map and G4 endpoint use");
            require(!slot.usable_in_current_active_frame,
                "G21 promoted a hypothetical Flow SpillSlot into the actual active frame");
        }
    }
    require(saw_edge_with_live_value && live_values == 43U &&
            result.stats.native_n113_emitter_actions_inserted == 0U &&
            result.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "G21 failed to retain the G4 live-value universe or changed insertion readiness");
}

void test_g21_frame_scratch_inventory_is_deterministic_and_byte_neutral() {
    const auto source = n113_emitter_pressure_source();
    const auto baseline = compile_with_n113_g21_frame_spill_scratch(source, false);
    const auto first = compile_with_n113_g21_frame_spill_scratch(source, true);
    const auto second = compile_with_n113_g21_frame_spill_scratch(source, true);
    require(baseline.succeeded() && first.succeeded() && second.succeeded() &&
            baseline.object_bytes == first.object_bytes && first.object_bytes == second.object_bytes,
        "G21 read-only frame-scratch inventory changed emitted COFF bytes");
    const auto& a_functions = first.stats.native_n113_frame_scratch_diagnostic_function_reports;
    const auto& b_functions = second.stats.native_n113_frame_scratch_diagnostic_function_reports;
    require(a_functions.size() == b_functions.size() && !a_functions.empty(),
        "G21 deterministic function inventory changed between identical runs");
    for (std::size_t f = 0U; f < a_functions.size(); ++f) {
        const auto& a = a_functions[f];
        const auto& b = b_functions[f];
        require(a.function_index == b.function_index && a.function_name == b.function_name &&
                a.active_frame_size_bytes == b.active_frame_size_bytes &&
                a.flow_spill_slots_planned == b.flow_spill_slots_planned &&
                a.edges.size() == b.edges.size(),
            "G21 frame/function identity or allocation totals are nondeterministic");
        for (std::size_t e = 0U; e < a.edges.size(); ++e) {
            const auto& x = a.edges[e];
            const auto& y = b.edges[e];
            require(x.edge_id == y.edge_id && x.g4_live_values == y.g4_live_values &&
                    x.flow_spill_slots.size() == y.flow_spill_slots.size(),
                "G21 per-edge frame-scratch inventory is nondeterministic");
            for (std::size_t slot = 0U; slot < x.flow_spill_slots.size(); ++slot) {
                const auto& left = x.flow_spill_slots[slot];
                const auto& right = y.flow_spill_slots[slot];
                require(left.slot_index == right.slot_index &&
                        left.hypothetical_frame_displacement == right.hypothetical_frame_displacement &&
                        left.live_endpoint_uses == right.live_endpoint_uses &&
                        left.candidate_after_flow_frame_reservation == right.candidate_after_flow_frame_reservation,
                    "G21 slot mapping, endpoint occupancy or eligibility changed between identical runs");
            }
        }
    }
    require(first.stats.native_n113_emitter_actions_inserted == 0U &&
            first.stats.native_n113_emitter_machine_moves_inserted == 0U &&
            first.stats.native_n113_emitter_transfer_bytes_inserted == 0U &&
            first.stats.native_n113_flow_edge_machine_ready_plans == 0U,
        "G21 determinism check observed an insertion or machine-readiness promotion");
}

void test_g16_address_and_value_reads_keep_distinct_emitter_roles() {
    auto fixture = make_g4_flow_edge_fixture();
    assign_g16_fixture_source_spans(fixture);
    fixture.emitter_locations.values.at(7U).address_taken = true;
    fixture.emitter_locations.values.at(7U).active_location =
        fixture.emitter_locations.values.at(7U).canonical_home;
    auto anchors = make_g8_operand_anchors(fixture);
    const auto read_anchor = std::find_if(anchors.begin(), anchors.end(), [](const auto& anchor) {
        return anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowRead;
    });
    require(read_anchor != anchors.end(), "G16 role fixture has no FlowRead anchor");
    read_anchor->emitted_operand_kind = X64FlowEmitterAnchor::OperandKind::AddressRead;
    read_anchor->emitted_operand_location = fixture.emitter_locations.values.at(7U).canonical_home;
    const auto plan = audit_g16_fixture(fixture, anchors);
    const auto& read = find_g16_operand(plan, NativeRegisterInstructionOriginKind::FlowRead);
    require(plan.succeeded && read.operand_kind == X64FlowEmitterAnchor::OperandKind::AddressRead &&
            read.flow_origin_kind == NativeRegisterInstructionOriginKind::FlowRead &&
            read.flow_position_kind == NativeX64FlowOperandPositionKind::BeforeOperation,
        "G16 collapsed an AddressRead into a value read or lost its source operation role");
}

void run_n113_flow_spill_reload_resolution_tests(int& passed, int& failed) {
    run_case("N113A_call_actions_keep_identity_and_placement",
        test_call_actions_convert_once_with_stable_identity_and_placement, passed, failed);
    run_case("N113B_gp_scratch_avoids_live_and_expression_registers",
        test_gp_temporary_avoids_live_registers_and_expression_reservations, passed, failed);
    run_case("N113C_xmm_scratch_is_class_correct_and_unoccupied",
        test_xmm_temporary_uses_matching_class_and_avoids_live_registers, passed, failed);
    run_case("N113D_duplicate_action_rejected_before_insertion",
        test_duplicate_action_is_rejected_before_insertion, passed, failed);
    run_case("N113E_action_placement_mismatch_rejected",
        test_mismatched_action_anchor_or_placement_is_rejected, passed, failed);
    run_case("N113F_failed_n112_plan_rejected",
        test_failed_n112_plan_is_rejected, passed, failed);
    run_case("N113G_no_safe_scratch_register_rejected",
        test_missing_safe_scratch_register_is_rejected, passed, failed);
    run_case("N113H_resolved_reload_rebases_rel32_and_coff_relocation",
        test_resolved_reload_insertion_rebases_branch_and_coff_relocation, passed, failed);
    run_case("N113I_scratch_selection_uses_allocator_register_pools",
        test_scratch_selection_stays_inside_allocator_register_pools, passed, failed);
    run_case("N113J_real_emitter_resolves_actions_to_stable_anchors",
        test_real_emitter_resolves_actions_to_call_read_write_and_cfg_anchors, passed, failed);
    run_case("N113K_emitter_anchor_proof_is_opt_in_and_byte_neutral",
        test_emitter_anchor_proof_is_opt_in_and_keeps_object_bytes_unchanged, passed, failed);
    run_case("N113L_location_audit_classifies_register_and_unmapped_endpoints",
        test_location_audit_distinguishes_register_matches_mismatches_and_unmapped_endpoints, passed, failed);
    run_case("N113M_location_audit_does_not_equate_equal_numbered_home_slots",
        test_location_audit_never_equates_same_numbered_n112_and_n111_home_slots, passed, failed);
    run_case("N113N_location_audit_rejects_duplicate_actions_and_invalid_plan",
        test_location_audit_rejects_duplicate_action_identity_and_invalid_plan, passed, failed);
    run_case("N113O_real_emitter_location_audit_is_opt_in_and_byte_neutral",
        test_real_emitter_location_audit_is_opt_in_accounted_and_byte_neutral, passed, failed);
    run_case("N113P_operation_location_audit_classifies_before_after_positions",
        test_operation_location_audit_classifies_before_after_positions, passed, failed);
    run_case("N113Q_operation_location_audit_rejects_duplicate_and_inconsistent_entries",
        test_operation_location_audit_rejects_duplicate_and_inconsistent_plan_entries, passed, failed);
    run_case("N113R_real_emitter_operation_location_audit_is_opt_in_and_byte_neutral",
        test_real_emitter_operation_location_audit_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113S_real_emitter_operand_crosswalk_is_opt_in_and_byte_neutral",
        test_real_emitter_operand_crosswalk_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113T_flow_frame_slots_are_bound_after_emitter_namespaces",
        test_flow_frame_slots_are_bound_after_emitter_namespaces, passed, failed);
    run_case("N113U_flow_frame_reservation_is_opt_in_and_drives_probe_unwind",
        test_flow_frame_reservation_is_opt_in_and_drives_probe_unwind, passed, failed);
    run_case("N113V_g1_call_actions_rebind_to_n111_home_and_rebase_flow_home",
        test_g1_call_actions_rebind_to_n111_home_and_rebase_flow_home, passed, failed);
    run_case("N113W_g1_definition_and_use_rebind_exact_emitter_operand_and_spill_namespace",
        test_g1_definition_and_use_rebind_exact_emitter_operand_and_spill_namespace, passed, failed);
    run_case("N113X_g1_rejects_missing_or_mismatched_emitter_operand_anchor",
        test_g1_rejects_missing_or_mismatched_emitter_operand_anchor, passed, failed);
    run_case("N113Y_g1_rejects_when_both_gp_and_xmm_scratch_banks_are_live",
        test_g1_rejects_when_both_gp_and_xmm_scratch_banks_are_live, passed, failed);
    run_case("N113Y2_g1_selects_independent_gp_scratch_per_action_boundary",
        test_g1_selects_independent_gp_scratch_per_action_boundary, passed, failed);
    run_case("N113Y5_g1_gp_memory_action_uses_dead_xmm_when_all_gp_registers_are_live",
        test_g1_gp_memory_action_uses_dead_xmm_when_all_gp_registers_are_live, passed, failed);
    run_case("N113Y3_g2_insertion_uses_each_binding_cycle_temporary",
        test_g2_insertion_uses_each_binding_cycle_temporary, passed, failed);
    run_case("N113Y4_g2_gp_memory_action_encodes_with_action_local_xmm_movq",
        test_g2_gp_memory_action_encodes_with_action_local_xmm_movq, passed, failed);
    run_case("N113AA_g1_xmm_memory_to_memory_uses_class_correct_scratch",
        test_g1_xmm_memory_to_memory_uses_class_correct_scratch, passed, failed);
    run_case("N113Z_g1_real_emitter_rebinding_is_opt_in_and_byte_neutral",
        test_g1_real_emitter_rebinding_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113AB_g2_repeated_value_actions_keep_operation_identity",
        test_g2_insertion_allows_same_value_action_at_distinct_operations, passed, failed);
    run_case("N113AC_g2_real_emitter_inserts_rebound_actions_and_rebases_anchors",
        test_g2_real_emitter_inserts_rebound_actions_and_rebases_anchors, passed, failed);
    run_case("N113AD_g3_g2_insertion_preserves_win64_unwind_records",
        test_g3_g2_insertion_preserves_win64_unwind_records, passed, failed);
    run_case("N113AE_g3_g2_object_keeps_valid_relocations_and_defined_entry_symbols",
        test_g3_g2_object_keeps_valid_relocations_and_defined_entry_symbols, passed, failed);
    run_case("N113AR_g5_real_emitter_edge_audit_is_opt_in_and_byte_neutral",
        test_g5_real_emitter_edge_audit_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113AS_g5_real_emitter_edge_audit_composes_with_g2_without_changing_moves",
        test_g5_real_emitter_edge_audit_composes_with_opt_in_g2_without_changing_g2_moves, passed, failed);
    run_case("N113AT_g5_edge_audit_uses_hypothetical_frame_without_reservation",
        test_g5_edge_audit_uses_hypothetical_flow_frame_without_enabling_reservation, passed, failed);
    run_case("N113AF_g4_audits_exact_diamond_edge_locations_and_home_phi_aliases",
        test_g4_audits_exact_diamond_edge_locations_and_home_phi_aliases, passed, failed);
    run_case("N113AG_g4_coalesced_n112_edge_segments_need_no_candidate_transition",
        test_g4_coalesced_n112_edge_segments_need_no_candidate_transition, passed, failed);
    run_case("N113AH_g4_maps_flow_spill_endpoints_without_equating_emitter_home_slots",
        test_g4_maps_flow_spill_endpoints_without_equating_emitter_home_slots, passed, failed);
    run_case("N113AI_g4_rejects_missing_flow_frame_mapping",
        test_g4_rejects_missing_flow_frame_mapping, passed, failed);
    run_case("N113AJ_g4_rejects_invalid_flow_frame_alignment",
        test_g4_rejects_invalid_flow_frame_alignment, passed, failed);
    run_case("N113AK_g4_rejects_duplicate_flow_edge_identity",
        test_g4_rejects_duplicate_flow_edge_identity, passed, failed);
    run_case("N113AL_g4_rejects_n111_location_missing_for_live_edge_value",
        test_g4_rejects_n111_location_missing_for_live_edge_value, passed, failed);
    run_case("N113AM_g4_rejects_phi_alias_that_changes_source_home_identity",
        test_g4_rejects_phi_alias_that_changes_source_home_identity, passed, failed);
    run_case("N113AN_g4_rejects_phi_incoming_from_non_predecessor",
        test_g4_rejects_phi_incoming_from_non_predecessor, passed, failed);
    run_case("N113AO_g4_rejects_missing_or_ambiguous_segment_at_edge_boundary",
        test_g4_rejects_missing_or_ambiguous_segment_at_edge_boundary, passed, failed);
    run_case("N113AP_g4_rejects_empty_block_with_live_through_value",
        test_g4_rejects_empty_block_with_live_through_value, passed, failed);
    run_case("N113AQ_g4_rejects_incomplete_flow_edge_identity_coverage",
        test_g4_rejects_incomplete_flow_edge_identity_coverage, passed, failed);
    run_case("N113AU_g6_plans_exact_diamond_edge_candidates_without_machine_readiness",
        test_g6_plans_exact_diamond_edge_candidates_without_claiming_machine_ready, passed, failed);
    run_case("N113AV_g6_coalesced_edge_locations_produce_no_candidates",
        test_g6_coalesced_edge_locations_produce_no_candidates, passed, failed);
    run_case("N113AW_g6_rejects_missing_exact_branch_anchor",
        test_g6_rejects_missing_exact_branch_anchor, passed, failed);
    run_case("N113AX_g6_rejects_duplicate_branch_anchor_identity",
        test_g6_rejects_duplicate_branch_anchor_identity, passed, failed);
    run_case("N113AY_g6_rejects_branch_anchor_with_mismatched_edge_endpoints",
        test_g6_rejects_branch_anchor_with_mismatched_edge_endpoints, passed, failed);
    run_case("N113AZ_g6_rejects_register_class_mismatch",
        test_g6_rejects_register_class_mismatch, passed, failed);
    run_case("N113BA_g6_rejects_temporary_flow_endpoint",
        test_g6_rejects_temporary_flow_endpoint, passed, failed);
    run_case("N113BB_g6_rejects_invalid_flow_frame_endpoint_mapping",
        test_g6_rejects_invalid_flow_frame_endpoint_mapping, passed, failed);
    run_case("N113BC_g7_schedules_acyclic_gp_copies_deterministically",
        test_g7_schedules_acyclic_gp_copies_deterministically, passed, failed);
    run_case("N113BD_g7_breaks_gp_parallel_copy_cycle_with_class_correct_temporary",
        test_g7_breaks_gp_parallel_copy_cycle_with_class_correct_temporary, passed, failed);
    run_case("N113BE_g7_schedules_gp_and_xmm_classes_independently",
        test_g7_schedules_gp_and_xmm_classes_independently, passed, failed);
    run_case("N113BF_g7_keeps_memory_to_memory_copy_for_later_encoder_expansion",
        test_g7_keeps_memory_to_memory_copy_as_a_scheduled_operation, passed, failed);
    run_case("N113BG_g7_rejects_duplicate_destinations_within_an_edge",
        test_g7_rejects_duplicate_destinations_across_one_edge, passed, failed);
    run_case("N113BH_g7_rejects_temporary_aliasing_edge_endpoint",
        test_g7_rejects_cycle_temporary_aliasing_an_edge_endpoint, passed, failed);
    run_case("N113BI_g7_rejects_temporary_outside_allocator_pools",
        test_g7_rejects_cycle_temporaries_outside_allocator_pools, passed, failed);
    run_case("N113BJ_g7_rejects_inconsistent_branch_anchor_records",
        test_g7_rejects_inconsistent_branch_anchor_records_for_one_edge, passed, failed);
    run_case("N113BK_g7_rejects_false_machine_insertion_readiness",
        test_g7_rejects_candidate_plan_that_claims_machine_insertion_ready, passed, failed);
    run_case("N113BL_g7_rejects_missing_authoritative_value_class",
        test_g7_rejects_missing_authoritative_value_class, passed, failed);
    run_case("N113BM_g7_distinguishes_gp_and_xmm_register_files",
        test_g7_distinguishes_gp_and_xmm_registers_with_the_same_index, passed, failed);
    run_case("N113BN_g8_reports_operand_owner_mismatch_but_proves_dead_scratch",
        test_g8_reports_operand_owner_mismatch_but_proves_dead_scratch, passed, failed);
    run_case("N113BO_g8_rejects_missing_final_branch_anchor",
        test_g8_rejects_missing_final_branch_anchor, passed, failed);
    run_case("N113BP_g8_rejects_duplicate_operation_anchor_identity",
        test_g8_rejects_duplicate_operation_anchor_identity, passed, failed);
    run_case("N113BQ_g8_rejects_emitted_operand_location_not_owned_by_n111",
        test_g8_rejects_emitted_operand_location_not_owned_by_n111, passed, failed);
    run_case("N113BR_g8_rejects_missing_concrete_value_operand_anchor_for_edge_owner",
        test_g8_rejects_missing_concrete_value_operand_anchor_for_edge_owner, passed, failed);
    run_case("N113BS_g8_checks_physical_frame_offsets_not_equal_slot_numbers",
        test_g8_checks_physical_frame_offsets_not_equal_slot_numbers, passed, failed);
    run_case("N113BT_g8_rejects_expression_reserved_memory_expansion_scratch",
        test_g8_rejects_expression_reserved_memory_expansion_scratch, passed, failed);
    run_case("N113BU_g8_rejects_scratch_occupied_by_coalesced_live_through_value",
        test_g8_rejects_scratch_occupied_by_coalesced_live_through_value, passed, failed);
    run_case("N113BV_g8_function_emitter_audit_is_opt_in_and_byte_neutral",
        test_g8_function_emitter_audit_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113BW_g8_function_emitter_audit_composes_with_g2_without_changing_moves",
        test_g8_function_emitter_audit_composes_with_g2_without_changing_moves, passed, failed);
    run_case("N113BX_g9_selects_distinct_gp_scratch_per_exact_edge",
        test_g9_selects_distinct_gp_scratch_per_exact_edge, passed, failed);
    run_case("N113BY_g9_keeps_each_schedule_bound_to_its_exact_branch_anchor",
        test_g9_keeps_each_schedule_bound_to_its_exact_branch_anchor, passed, failed);
    run_case("N113BZ_g9_selects_xmm_scratch_independently_from_gp_register_file",
        test_g9_selects_xmm_scratch_independently_from_gp_register_file, passed, failed);
    run_case("N113CA_g9_fails_closed_when_expression_reserves_every_gp_scratch",
        test_g9_fails_closed_when_expression_reserves_every_gp_scratch, passed, failed);
    run_case("N113CB_g9_rejects_candidate_plan_claiming_machine_readiness",
        test_g9_rejects_candidate_plan_claiming_machine_readiness, passed, failed);
    run_case("N113CC_g9_scratch_selection_is_deterministic",
        test_g9_scratch_selection_is_deterministic, passed, failed);
    run_case("N113CD_g9_function_emitter_planner_is_opt_in_and_byte_neutral",
        test_g9_function_emitter_planner_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113CE_g9_per_edge_audit_composes_with_g2_without_changing_moves",
        test_g9_per_edge_audit_composes_with_g2_without_changing_moves, passed, failed);
    run_case("N113CF_g10_reports_exact_n112_to_emitter_operand_mismatches",
        test_g10_reports_exact_n112_to_emitter_operand_mismatches, passed, failed);
    run_case("N113CG_g10_proves_each_n112_operation_operand_when_physical_location_matches",
        test_g10_proves_each_n112_operation_operand_when_physical_location_matches, passed, failed);
    run_case("N113CH_g10_compares_flow_and_emitter_memory_by_physical_frame_offset",
        test_g10_compares_flow_and_emitter_memory_by_physical_frame_offset, passed, failed);
    run_case("N113CI_g10_rejects_same_numbered_slots_with_different_physical_offsets",
        test_g10_rejects_same_numbered_slots_with_different_physical_offsets, passed, failed);
    run_case("N113CJ_g10_rejects_duplicate_operation_anchors",
        test_g10_rejects_duplicate_operation_anchors, passed, failed);
    run_case("N113CK_g10_rejects_anchor_from_another_function_index",
        test_g10_rejects_anchor_from_another_function_index, passed, failed);
    run_case("N113CL_g10_rejects_missing_concrete_operation_anchor",
        test_g10_rejects_missing_concrete_operation_anchor, passed, failed);
    run_case("N113CM_g10_rejects_missing_n112_operand_position",
        test_g10_rejects_missing_n112_operand_position, passed, failed);
    run_case("N113CN_g10_audits_address_reads_against_the_canonical_home",
        test_g10_audits_address_reads_against_the_canonical_home, passed, failed);
    run_case("N113CO_g10_function_emitter_audit_is_opt_in_and_byte_neutral",
        test_g10_function_emitter_audit_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113CP_g10_crosswalk_composes_with_g2_and_g9_without_changing_moves",
        test_g10_crosswalk_composes_with_g2_and_g9_without_changing_moves, passed, failed);
    run_case("N113CQ_g11_passes_only_when_g8_g9_and_g10_proofs_intersect",
        test_g11_passes_only_when_g8_g9_and_g10_proofs_intersect, passed, failed);
    run_case("N113CR_g11_blocks_edge_when_g9_scratch_is_not_proven",
        test_g11_blocks_edge_when_g9_scratch_is_not_proven, passed, failed);
    run_case("N113CS_g11_blocks_edge_when_g8_endpoint_ownership_is_incomplete",
        test_g11_blocks_edge_when_g8_endpoint_ownership_is_incomplete, passed, failed);
    run_case("N113CT_g11_blocks_edge_when_candidate_value_has_no_g10_operand_evidence",
        test_g11_blocks_edge_when_candidate_value_has_no_g10_operand_evidence, passed, failed);
    run_case("N113CU_g11_blocks_edge_when_candidate_value_has_any_g10_mismatch",
        test_g11_blocks_edge_when_candidate_value_has_any_g10_mismatch, passed, failed);
    run_case("N113CV_g11_ignores_unrelated_g10_mismatch_on_another_value",
        test_g11_ignores_unrelated_g10_mismatch_on_another_value, passed, failed);
    run_case("N113CW_g11_rejects_cross_function_g10_crosswalk",
        test_g11_rejects_cross_function_g10_crosswalk, passed, failed);
    run_case("N113CX_g11_rejects_g9_schedule_with_wrong_edge_identity",
        test_g11_rejects_g9_schedule_with_wrong_edge_identity, passed, failed);
    run_case("N113CY_g11_rejects_schedule_missing_one_candidate_value",
        test_g11_rejects_schedule_missing_one_candidate_value, passed, failed);
    run_case("N113CZ_g11_function_emitter_audit_is_opt_in_and_byte_neutral",
        test_g11_function_emitter_audit_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113DA_g11_eligibility_composes_with_g2_without_changing_moves_or_bytes",
        test_g11_eligibility_composes_with_g2_without_changing_moves_or_bytes, passed, failed);
    run_case("N113DB_g12_emits_one_function_report_per_function_when_enabled",
        test_g12_emits_one_function_report_per_function_when_enabled, passed, failed);
    run_case("N113DC_g12_per_function_counters_sum_to_g11_aggregate_stats",
        test_g12_per_function_counters_sum_to_g11_aggregate_stats, passed, failed);
    run_case("N113DD_g12_edge_reports_retain_exact_ids_and_blocker_conjunction",
        test_g12_edge_reports_retain_exact_ids_and_blocker_conjunction, passed, failed);
    run_case("N113DE_g12_no_per_function_reports_when_g11_disabled",
        test_g12_no_per_function_reports_when_g11_disabled, passed, failed);
    run_case("N113DF_g12_function_and_edge_report_order_is_deterministic",
        test_g12_function_and_edge_report_order_is_deterministic, passed, failed);
    run_case("N113DG_g12_per_function_census_is_byte_neutral",
        test_g12_per_function_census_is_byte_neutral, passed, failed);
    run_case("N113DH_g12_census_composes_with_g2_without_changing_inserted_moves",
        test_g12_census_composes_with_g2_without_changing_inserted_moves, passed, failed);
    run_case("N113DI_g12_reports_are_isolated_between_compile_results",
        test_g12_reports_are_isolated_between_compile_results, passed, failed);
    run_case("N113DJ_g12_each_report_has_consistent_blocked_reason_counters",
        test_g12_each_report_has_consistent_blocked_reason_counters, passed, failed);
    run_case("N113DK_g12_report_diagnostics_preserve_g11_failure_evidence",
        test_g12_report_diagnostics_preserve_g11_failure_evidence, passed, failed);
    run_case("N113DL_g12_census_does_not_claim_machine_insertion_readiness",
        test_g12_census_does_not_claim_machine_insertion_readiness, passed, failed);
    run_case("N113DM_g12_edge_reports_preserve_value_coverage_details",
        test_g12_edge_reports_preserve_value_coverage_details, passed, failed);
    run_case("N113DN_g12_reports_real_emitter_candidate_inventory_and_blockers",
        test_g12_reports_real_emitter_candidate_inventory_and_blockers, passed, failed);
    run_case("N113DO_g13_reports_each_candidate_value_with_independent_operand_evidence",
        test_g13_reports_each_candidate_value_with_independent_operand_evidence, passed, failed);
    run_case("N113DP_g13_reports_missing_g10_evidence_per_value",
        test_g13_reports_missing_g10_evidence_per_value, passed, failed);
    run_case("N113DQ_g13_retains_all_operation_ids_for_a_candidate_value",
        test_g13_retains_all_operation_ids_for_a_candidate_value, passed, failed);
    run_case("N113DR_g13_preserves_exact_mismatch_operation_and_emitter_location",
        test_g13_preserves_exact_mismatch_operation_and_emitter_location, passed, failed);
    run_case("N113DS_g13_proven_value_count_requires_all_observed_operations_to_match",
        test_g13_proven_value_count_requires_all_observed_operations_to_match, passed, failed);
    run_case("N113DT_g13_preserves_physical_frame_displacement_mismatch",
        test_g13_preserves_physical_frame_displacement_mismatch, passed, failed);
    run_case("N113DU_g13_orders_value_and_operation_evidence_deterministically",
        test_g13_orders_value_and_operation_evidence_deterministically, passed, failed);
    run_case("N113DV_g13_function_emitter_reports_value_level_g10_blockers",
        test_g13_function_emitter_reports_value_level_g10_blockers, passed, failed);
    run_case("N113DW_g13_census_value_evidence_remains_byte_neutral",
        test_g13_census_value_evidence_remains_byte_neutral, passed, failed);
    run_case("N113DX_g13_composes_with_g2_without_changing_machine_moves_or_bytes",
        test_g13_composes_with_g2_without_changing_machine_moves_or_bytes, passed, failed);
    run_case("N113DY_g13_does_not_claim_insertion_readiness",
        test_g13_does_not_claim_insertion_readiness, passed, failed);
    run_case("N113DZ_g14_classifies_register_index_mismatch",
        test_g14_classifies_register_index_mismatch, passed, failed);
    run_case("N113EA_g14_classifies_invalid_register_mismatch",
        test_g14_classifies_invalid_register_mismatch, passed, failed);
    run_case("N113EB_g14_classifies_flow_register_to_emitter_memory",
        test_g14_classifies_flow_register_to_emitter_memory, passed, failed);
    run_case("N113EC_g14_classifies_flow_memory_to_emitter_register",
        test_g14_classifies_flow_memory_to_emitter_register, passed, failed);
    run_case("N113ED_g14_classifies_physical_frame_displacement_mismatch",
        test_g14_classifies_physical_frame_displacement_mismatch, passed, failed);
    run_case("N113EE_g14_classifies_unavailable_frame_mapping",
        test_g14_classifies_unavailable_frame_mapping, passed, failed);
    run_case("N113EF_g14_classifies_inconsistent_match_flag_as_evidence_disagreement",
        test_g14_classifies_inconsistent_match_flag_as_evidence_disagreement, passed, failed);
    run_case("N113EG_g14_classifies_unsupported_location_kind",
        test_g14_classifies_unsupported_location_kind, passed, failed);
    run_case("N113EH_g14_real_function_reports_aggregate_mismatch_categories",
        test_g14_real_function_reports_aggregate_mismatch_categories, passed, failed);
    run_case("N113EI_g14_mismatch_classification_is_deterministic_and_byte_neutral",
        test_g14_mismatch_classification_is_deterministic_and_byte_neutral, passed, failed);
    run_case("N113EJ_g14_composes_with_g2_without_changing_machine_moves_or_bytes",
        test_g14_composes_with_g2_without_changing_machine_moves_or_bytes, passed, failed);
    run_case("N113EK_g15_classifies_register_class_mismatch_with_equal_register_index",
        test_g15_classifies_register_class_mismatch_with_equal_register_index, passed, failed);
    run_case("N113EL_g15_register_class_mismatch_precedes_register_index_difference",
        test_g15_register_class_mismatch_precedes_register_index_difference, passed, failed);
    run_case("N113EM_g15_register_class_mismatch_precedes_register_memory_difference",
        test_g15_register_class_mismatch_precedes_register_memory_difference, passed, failed);
    run_case("N113EN_g15_matching_register_classes_keep_g14_location_categories",
        test_g15_matching_register_classes_keep_g14_location_categories, passed, failed);
    run_case("N113EO_g15_operation_report_exposes_flow_and_candidate_classes",
        test_g15_operation_report_exposes_flow_and_candidate_classes, passed, failed);
    run_case("N113EP_g15_class_mismatch_counts_reconcile_value_edge_and_function",
        test_g15_class_mismatch_counts_reconcile_value_edge_and_function, passed, failed);
    run_case("N113EQ_g15_class_mismatch_is_distinct_from_invalid_register",
        test_g15_class_mismatch_is_distinct_from_invalid_register, passed, failed);
    run_case("N113ER_g15_register_class_mismatch_is_deterministic",
        test_g15_register_class_mismatch_is_deterministic, passed, failed);
    run_case("N113ES_g15_classification_remains_byte_neutral",
        test_g15_classification_remains_byte_neutral, passed, failed);
    run_case("N113ET_g15_classification_composes_with_g2_without_changing_moves",
        test_g15_classification_composes_with_g2_without_changing_moves, passed, failed);
    run_case("N113EU_g15_does_not_claim_edge_bundle_insertion_readiness",
        test_g15_does_not_claim_edge_bundle_insertion_readiness, passed, failed);
    run_case("N113EV_g15_g10_crosswalk_preserves_register_class_mismatch_as_evidence",
        test_g15_g10_crosswalk_preserves_register_class_mismatch_as_evidence, passed, failed);
    run_case("N113FI_g17_function_report_preserves_actual_flow_register_class",
        test_g17_function_report_preserves_actual_flow_register_class, passed, failed);
    run_case("N113FJ_g17_flow_and_candidate_register_classes_are_both_projected",
        test_g17_flow_and_candidate_register_classes_are_both_projected, passed, failed);
    run_case("N113EW_g16_flow_read_reports_before_position_and_source_span",
        test_g16_flow_read_reports_before_position_and_source_span, passed, failed);
    run_case("N113EX_g16_flow_write_reports_after_position",
        test_g16_flow_write_reports_after_position, passed, failed);
    run_case("N113EY_g16_flow_declare_reports_after_position",
        test_g16_flow_declare_reports_after_position, passed, failed);
    run_case("N113EZ_g16_address_read_reports_before_position_and_role",
        test_g16_address_read_reports_before_position_and_role, passed, failed);
    run_case("N113FA_g16_reports_candidate_and_actual_flow_register_classes",
        test_g16_reports_candidate_and_actual_flow_register_classes, passed, failed);
    run_case("N113FB_g16_source_span_disagreement_is_visible_but_nonfatal",
        test_g16_source_span_disagreement_is_visible_but_nonfatal, passed, failed);
    run_case("N113FC_g16_function_edge_reports_carry_operation_provenance",
        test_g16_function_edge_reports_carry_operation_provenance, passed, failed);
    run_case("N113FD_g16_provenance_is_deterministic",
        test_g16_provenance_is_deterministic, passed, failed);
    run_case("N113FE_g16_provenance_is_opt_in_and_byte_neutral",
        test_g16_provenance_is_opt_in_and_byte_neutral, passed, failed);
    run_case("N113FF_g16_provenance_composes_with_g2_without_changing_moves",
        test_g16_provenance_composes_with_g2_without_changing_moves, passed, failed);
    run_case("N113FG_g16_provenance_does_not_promote_insertion_readiness",
        test_g16_provenance_does_not_promote_insertion_readiness, passed, failed);
    run_case("N113FH_g16_operation_evidence_exposes_exact_candidate_class_separately",
        test_g16_operation_evidence_exposes_exact_candidate_class_separately, passed, failed);
    run_case("N113FK_g19_g8_exports_every_live_value_endpoint_and_reconciles_counts",
        test_g19_g8_exports_every_live_value_endpoint_and_reconciles_counts, passed, failed);
    run_case("N113FL_g19_function_emitter_report_projects_endpoint_records",
        test_g19_function_emitter_report_projects_endpoint_records, passed, failed);
    run_case("N113FM_g19_endpoint_projection_is_deterministic_and_byte_neutral",
        test_g19_endpoint_projection_is_deterministic_and_byte_neutral, passed, failed);
    run_case("N113FN_g20_g4_live_values_reconcile_with_g6_candidates",
        test_g20_g4_live_values_reconcile_with_g6_candidates, passed, failed);
    run_case("N113FO_g20_g7_trials_explain_when_g8_was_not_attempted",
        test_g20_g7_trials_explain_when_g8_was_not_attempted, passed, failed);
    run_case("N113FP_g20_public_projection_joins_g10_and_g11_without_gate_changes",
        test_g20_public_projection_joins_g10_and_g11_without_gate_changes, passed, failed);
    run_case("N113FQ_g20_diagnostics_are_deterministic_and_byte_neutral",
        test_g20_diagnostics_are_deterministic_and_byte_neutral, passed, failed);
    run_case("N113FU_g22_resolver_supports_dedicated_spill_slot_cycle_storage",
        test_g22_resolver_supports_dedicated_spill_slot_cycle_storage, passed, failed);
    run_case("N113FV_g22_resolver_rejects_home_and_aliased_cycle_storage",
        test_g22_resolver_rejects_home_and_aliased_cycle_storage, passed, failed);
    run_case("N113FW_g22_gp_memory_cycle_encoding_uses_separate_expansion_register",
        test_g22_gp_memory_cycle_encoding_uses_separate_expansion_register, passed, failed);
    run_case("N113FX_g22_xmm_memory_cycle_encoding_uses_separate_expansion_register",
        test_g22_xmm_memory_cycle_encoding_uses_separate_expansion_register, passed, failed);
    run_case("N113FY_g22_encoder_rejects_expansion_register_and_frame_slot_aliases",
        test_g22_encoder_rejects_expansion_register_and_frame_slot_aliases, passed, failed);
    run_case("N113FZ_g24_gp_memory_cycle_uses_xmm_movq_without_cross_bank_alias_false_positive",
        test_g24_gp_memory_cycle_uses_xmm_movq_without_cross_bank_alias_false_positive, passed, failed);
    run_case("N113GA_g24_gp_movq_expansion_rejects_fixed_and_nonvolatile_xmm_registers",
        test_g24_gp_movq_expansion_rejects_fixed_and_nonvolatile_xmm_registers, passed, failed);
    run_case("N113GB_g24_gp_movq_expansion_rejects_wrong_class_and_untyped_temporary",
        test_g24_gp_movq_expansion_rejects_wrong_class_and_untyped_temporary, passed, failed);
    run_case("N113GC_g25_reconciles_g4_live_sets_and_selects_first_dead_volatile_xmm",
        test_g25_reconciles_g4_live_sets_and_selects_first_dead_volatile_xmm, passed, failed);
    run_case("N113GD_g25_gp_numeric_register_index_does_not_alias_same_xmm_index",
        test_g25_gp_numeric_register_index_does_not_alias_same_xmm_index, passed, failed);
    run_case("N113GE_g25_rejects_missing_g4_live_endpoint_record",
        test_g25_rejects_missing_g4_live_endpoint_record, passed, failed);
    run_case("N113GF_g25_rejects_duplicate_g4_value_endpoint_record",
        test_g25_rejects_duplicate_g4_value_endpoint_record, passed, failed);
    run_case("N113GG_g25_rejects_g6_candidate_that_disagrees_with_g4_endpoint",
        test_g25_rejects_g6_candidate_that_disagrees_with_g4_endpoint, passed, failed);
    run_case("N113GH_g25_is_deterministic_and_never_promotes_insertion_readiness",
        test_g25_is_deterministic_and_never_promotes_insertion_readiness, passed, failed);
    run_case("N113GI_g25_rejects_g4_live_value_counter_mismatch",
        test_g25_rejects_g4_live_value_counter_mismatch, passed, failed);
    run_case("N113GJ_g25_rejects_entirely_omitted_g6_candidate_edge",
        test_g25_rejects_entirely_omitted_g6_candidate_edge, passed, failed);
    run_case("N113GK_g26_reconciles_multiple_edges_and_reuses_edge_local_cycle_storage",
        test_g26_reconciles_multiple_edges_and_reuses_edge_local_cycle_storage, passed, failed);
    run_case("N113GL_g26_accepts_two_sequential_cycles_on_one_edge",
        test_g26_accepts_two_sequential_cycles_on_one_edge, passed, failed);
    run_case("N113GM_g26_rejects_cycle_restore_before_save",
        test_g26_rejects_cycle_restore_before_save, passed, failed);
    run_case("N113GN_g26_rejects_cycle_slot_overwrite_before_restore",
        test_g26_rejects_cycle_slot_overwrite_before_restore, passed, failed);
    run_case("N113GO_g26_rejects_cycle_restore_value_identity_mismatch",
        test_g26_rejects_cycle_restore_value_identity_mismatch, passed, failed);
    run_case("N113GP_g26_rejects_cycle_marker_without_spill_slot_access",
        test_g26_rejects_cycle_marker_without_spill_slot_access, passed, failed);
    run_case("N113GQ_g26_rejects_missing_or_duplicate_exact_edge_schedule",
        test_g26_rejects_missing_or_duplicate_exact_edge_schedule, passed, failed);
    run_case("N113GR_g26_rejects_unmapped_or_physically_aliased_cycle_slot",
        test_g26_rejects_unmapped_or_physically_aliased_cycle_slot, passed, failed);
    run_case("N113FR_g21_inventory_separates_active_emitter_and_hypothetical_flow_slots",
        test_g21_inventory_separates_active_emitter_and_hypothetical_flow_slots, passed, failed);
    run_case("N113FS_g21_flow_spill_candidate_requires_no_g4_live_endpoint_alias",
        test_g21_flow_spill_candidate_requires_no_g4_live_endpoint_alias, passed, failed);
    run_case("N113FT_g21_frame_scratch_inventory_is_deterministic_and_byte_neutral",
        test_g21_frame_scratch_inventory_is_deterministic_and_byte_neutral, passed, failed);
}
