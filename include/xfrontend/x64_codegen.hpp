// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/binary_object.hpp"
#include "xfrontend/native_object_emitter.hpp"
#include "xfrontend/native_register_analysis.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xfrontend {

struct X64ExternalFunctionSignature {
    std::string module_name;
    std::string name;
    std::string return_type;
    std::vector<std::string> parameter_types;
    std::string mangled_name;
    std::string signature_key;
};

struct X64CodegenOptions {
    std::string object_id = "x64-native-object";
    ObjectFileDescriptor target = make_coff_x86_64_object_target();
    // Set only after a whole-program semantic pass has validated every module.
    bool skip_semantic_validation = false;
    std::vector<X64ExternalFunctionSignature> imported_functions;
    // N113 experiment: validate the N112 spill/reload plan against emitted
    // Flow anchors without inserting or changing machine-code bytes.
    bool validate_n113_flow_action_anchors = false;
    // N113-C experiment: compare N112 action endpoints with N111 active
    // persistent locations; this is audit-only and never inserts code.
    bool validate_n113_flow_action_locations = false;
    // N113-D experiment: compare per-operation N112 positions to N111 active
    // locations; this is audit-only and never rewrites operands or code.
    bool validate_n113_flow_operation_locations = false;
    // N113-E experiment: cross-check each source Flow operation with the
    // concrete emitter operand location and machine-code boundary.
    bool validate_n113_flow_emitter_operand_locations = false;
    // N113-F experiment: reserve N112 Flow spill/home frame slots, without
    // inserting spill/reload instructions or rewriting emitter operands.
    bool reserve_n113_flow_frame_slots = false;
    // N113-G1: build a planner-only rebinding from N112 actions to exact N111
    // emitter locations and disjoint physical frame-map keys. No code is inserted.
    bool plan_n113_emitter_action_rebinding = false;
    // N113-G2: opt-in materialization of the G1-rebound spill/reload actions.
    // Enabling this also reserves the N112 Flow frame slots and rebases all
    // internal rel32 and external COFF relocation offsets after insertion.
    bool insert_n113_emitter_spill_reload_actions = false;
    // N113-G5: run the G4 CFG edge-location crosswalk inside the real
    // FunctionEmitter. The audit uses a hypothetical combined Flow frame map
    // when N112 slots are not reserved; it does not enlarge the active frame,
    // insert code, or change anchors/relocations/unwind data.
    bool audit_n113_flow_edge_locations = false;
    // N113-G8: prove exact N112-to-emitted-operand ownership and per-edge
    // liveness of the G7 cycle/memory-expansion scratch registers. Read-only.
    bool audit_n113_flow_edge_schedule_readiness = false;
    // N113-G9: independently select/prove GP and XMM cycle/memory-expansion
    // scratch on each exact CFG edge. Read-only; never inserts edge copies.
    bool plan_n113_flow_edge_scratch_per_edge = false;
    // N113-G10: cross-check every concrete FlowRead/FlowWrite/FlowDeclare
    // operand against the exact N112 before/after operation location.
    // Read-only evidence; mismatches are reported without changing code.
    bool audit_n113_flow_operation_operand_ownership = false;
    // N113-G11: intersect G6 exact-edge candidates with the G9 per-edge
    // scratch proof and G10 operation-operand crosswalk. Read-only; no edge
    // bundle is inserted, even when an edge passes this evidence gate.
    bool audit_n113_flow_edge_bundle_eligibility = false;
    // N113-G21: inventory active Emitter frame spill/home slots and the
    // hypothetical N112 Flow frame slots. Reconcile Flow spill-slot use with
    // exact G4 live-through endpoints. This is diagnostic only: it does not
    // reserve Flow slots, change frame size/unwind, or permit memory scratch.
    bool audit_n113_frame_spill_scratch = false;

    X64CodegenOptions() = default;
    X64CodegenOptions(std::string id, ObjectFileDescriptor descriptor,
                      bool skip_validation = false,
                      std::vector<X64ExternalFunctionSignature> imports = {})
        : object_id(std::move(id)), target(std::move(descriptor)),
          skip_semantic_validation(skip_validation), imported_functions(std::move(imports)) {}
};

// N113-G16: records whether an audited operand consumes the N112 location
// immediately before or immediately after its source Flow operation.
enum class NativeX64FlowOperandPositionKind : std::uint8_t {
    BeforeOperation,
    AfterOperation
};

enum class NativeX64FlowOperandMismatchKind : std::uint8_t {
    None,
    RegisterIndexMismatch,
    InvalidRegister,
    FlowRegisterEmitterMemory,
    FlowMemoryEmitterRegister,
    FrameDisplacementMismatch,
    FrameMappingUnavailable,
    EvidenceDisagreement,
    UnsupportedLocationKind,
    // G15: the N112 operation's authoritative register class disagrees with
    // the candidate ValueId class. This takes precedence over location-kind
    // and register-index classification.
    RegisterClassMismatch
};

// Counts mismatch evidence occurrences, not unique FlowOperationIds. The same
// operation may legitimately be referenced by more than one candidate edge.
struct NativeX64FlowOperandMismatchCounts {
    std::uint32_t register_index = 0U;
    std::uint32_t invalid_register = 0U;
    std::uint32_t flow_register_emitter_memory = 0U;
    std::uint32_t flow_memory_emitter_register = 0U;
    std::uint32_t frame_displacement = 0U;
    std::uint32_t frame_mapping_unavailable = 0U;
    std::uint32_t evidence_disagreement = 0U;
    std::uint32_t unsupported_location = 0U;
    std::uint32_t register_class = 0U;
};

// N113-G12: stable, read-only export of each FunctionEmitter's G11 edge census.
// These records describe proof evidence only; they are not insertion commands.
struct X64FlowEdgeBundleOperandEvidenceReport {
    std::uint64_t flow_operation_id = 0U;
    std::uint32_t flow_block_id = 0U;
    std::uint32_t flow_operation_index = 0U;
    std::uint8_t operand_kind = 0U;
    NativeRegisterClass flow_register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterClass candidate_value_register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterInstructionOriginKind flow_origin_kind = NativeRegisterInstructionOriginKind::Synthetic;
    NativeX64FlowOperandPositionKind flow_position_kind = NativeX64FlowOperandPositionKind::BeforeOperation;
    SourceSpan source_span{};
    bool source_span_matches_emitter_anchor = false;
    NativeRegisterLocation flow_operation_location;
    NativeRegisterLocation emitter_operand_location;
    bool flow_location_uses_frame = false;
    std::int32_t flow_frame_displacement = 0;
    bool emitter_location_uses_frame = false;
    std::int32_t emitter_frame_displacement = 0;
    bool location_matches = false;
    NativeX64FlowOperandMismatchKind mismatch_kind = NativeX64FlowOperandMismatchKind::None;
};

struct X64FlowEdgeValueOperandEvidenceReport {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    bool concrete_evidence_found = false;
    std::uint32_t operand_operations = 0U;
    std::uint32_t matching_operations = 0U;
    std::uint32_t mismatching_operations = 0U;
    bool all_observed_operands_match = false;
    NativeX64FlowOperandMismatchCounts mismatch_counts;
    std::vector<X64FlowEdgeBundleOperandEvidenceReport> operations;
};

// N113-G19: public, read-only projection of G8's exact per-Value edge endpoint evidence.
struct X64FlowEdgeEndpointValueEvidenceReport {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterLocation predecessor_flow_location;
    NativeRegisterLocation successor_flow_location;
    NativeRegisterLocation emitter_active_location;
    bool predecessor_uses_frame = false;
    std::int32_t predecessor_frame_displacement = 0;
    bool successor_uses_frame = false;
    std::int32_t successor_frame_displacement = 0;
    bool emitter_location_is_home_slot = false;
    bool emitter_frame_mapping_available = false;
    std::int32_t emitter_frame_displacement = 0;
    bool predecessor_matches_emitter = false;
    bool successor_matches_emitter = false;
    std::uint32_t function_operand_anchor_count = 0U;
};

struct X64FlowEdgeBundleEligibilityEdgeReport {
    std::uint64_t edge_id = 0U;
    std::uint32_t predecessor_block_id = 0U;
    std::uint32_t successor_block_id = 0U;
    std::uint32_t candidate_values = 0U;
    std::uint32_t operation_values_proven = 0U;
    std::uint32_t operation_values_with_evidence = 0U;
    std::uint32_t operation_values_mismatched = 0U;
    std::uint32_t operation_values_missing = 0U;
    NativeX64FlowOperandMismatchCounts mismatch_counts;
    std::vector<X64FlowEdgeValueOperandEvidenceReport> value_operand_evidence;
    std::uint32_t live_values_audited = 0U;
    std::uint32_t operand_anchors_validated = 0U;
    std::uint32_t endpoint_ownership_matches = 0U;
    std::uint32_t endpoint_ownership_mismatches = 0U;
    std::vector<X64FlowEdgeEndpointValueEvidenceReport> endpoint_value_evidence;
    bool exact_g9_schedule = false;
    bool endpoint_ownership_proven = false;
    bool scratch_liveness_proven = false;
    bool operation_operand_ownership_proven = false;
    bool proof_gate_passed = false;
    std::vector<std::uint32_t> value_ids;
    std::vector<std::string> blockers;
};

struct X64FlowEdgeBundleEligibilityFunctionReport {
    std::size_t function_index = static_cast<std::size_t>(-1);
    std::string function_name;
    bool plan_succeeded = false;
    std::uint32_t candidate_edges = 0U;
    std::uint32_t proof_gate_passed_edges = 0U;
    std::uint32_t blocked_edges = 0U;
    std::uint32_t schedule_blocked_edges = 0U;
    std::uint32_t endpoint_ownership_blocked_edges = 0U;
    std::uint32_t scratch_blocked_edges = 0U;
    std::uint32_t operation_operand_blocked_edges = 0U;
    NativeX64FlowOperandMismatchCounts mismatch_counts;
    std::vector<X64FlowEdgeBundleEligibilityEdgeReport> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};


// N113-G21: allocation owner for an existing spill slot. The interval is in
// its allocator's position namespace; Flow and expression positions are not
// interchangeable.
struct X64FlowFrameSpillSlotOwnerDiagnosticReport {
    std::uint32_t segment_id = 0U;
    std::uint32_t value_id = 0U;
    std::uint32_t block_id = 0U;
    std::uint32_t start_position = 0U;
    std::uint32_t end_position = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
};

struct X64FlowEmitterSpillSlotDiagnosticReport {
    std::uint32_t slot_index = 0U;
    std::int32_t active_frame_displacement = 0;
    std::vector<X64FlowFrameSpillSlotOwnerDiagnosticReport> owners;
    // Expression allocation intervals currently have no exact FlowEdgeId
    // crosswalk. A nonempty owner set therefore cannot be declared edge-dead.
    bool edge_liveness_crosswalk_available = false;
    bool proven_free_for_edge_scratch = false;
};

struct X64FlowEdgeFlowSpillSlotDiagnosticReport {
    std::uint32_t slot_index = 0U;
    std::int32_t hypothetical_frame_displacement = 0;
    bool frame_mapping_available = false;
    bool active_frame_reserved = false;
    std::vector<X64FlowFrameSpillSlotOwnerDiagnosticReport> owners;
    std::uint32_t live_endpoint_uses = 0U;
    std::vector<std::uint32_t> live_endpoint_value_ids;
    bool no_g4_live_endpoint_alias = false;
    // True means the N112 slot is allocator-owned, physically mapped in the
    // hypothetical combined frame, and not used by any G4 live-through
    // predecessor/successor endpoint on this exact EdgeId. It is not active
    // until reserve_n113_flow_frame_slots is enabled in a separate milestone.
    bool candidate_after_flow_frame_reservation = false;
    bool usable_in_current_active_frame = false;
    // Current G7/G8/encoder cycle-temporary contract accepts registers only.
    bool current_g7_accepts_spill_slot_temporary = false;
};

struct X64FlowEdgeFrameScratchDiagnosticEdgeReport {
    std::uint64_t edge_id = 0U;
    std::uint32_t predecessor_block_id = 0U;
    std::uint32_t successor_block_id = 0U;
    std::uint32_t g4_live_values = 0U;
    std::vector<X64FlowEdgeFlowSpillSlotDiagnosticReport> flow_spill_slots;
};

struct X64FlowFrameScratchDiagnosticFunctionReport {
    std::size_t function_index = static_cast<std::size_t>(-1);
    std::string function_name;
    bool active_frame_plan_succeeded = false;
    std::uint32_t active_frame_size_bytes = 0U;
    std::uint32_t active_emitter_spill_slot_count = 0U;
    std::uint32_t active_emitter_home_slot_count = 0U;
    bool flow_frame_slots_reserved_in_active_frame = false;
    std::uint32_t flow_spill_slots_planned = 0U;
    std::uint32_t flow_home_slots_planned = 0U;
    std::uint32_t hypothetical_flow_spill_slots_mapped = 0U;
    std::vector<X64FlowEmitterSpillSlotDiagnosticReport> emitter_spill_slots;
    std::vector<X64FlowEdgeFrameScratchDiagnosticEdgeReport> edges;
};

// N113-G20: independent, read-only reconciliation of G4/G6/G7/G8/G9/G10/G11
// per exact edge. This is deliberately separate from the G11 eligibility
// report so its evidence does not alter readiness or proof-gate semantics.
struct X64FlowEdgeG4LiveValueDiagnosticReport {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterLocation predecessor_flow_location;
    NativeRegisterLocation successor_flow_location;
    NativeRegisterLocation emitter_active_location;
    bool predecessor_uses_frame = false;
    std::int32_t predecessor_frame_displacement = 0;
    bool successor_uses_frame = false;
    std::int32_t successor_frame_displacement = 0;
    bool flow_location_changes = false;
    bool is_g6_candidate = false;
};

struct X64FlowEdgeG6CandidateDiagnosticReport {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterLocation source_location;
    NativeRegisterLocation destination_location;
    NativeRegisterLocation emitter_active_location;
};

struct X64FlowEdgeG7ScratchTrialDiagnosticReport {
    std::uint8_t gp_temporary_register = 0U;
    std::uint8_t xmm_temporary_register = 0U;
    bool g7_schedule_succeeded = false;
    std::uint32_t scheduled_operations = 0U;
    std::string g7_rejection_code;
    std::string g7_rejection_message;
    bool g8_readiness_attempted = false;
    bool g8_readiness_audit_completed = false;
    std::string g8_rejection_code;
    std::string g8_rejection_message;
    bool scratch_liveness_proven = false;
    std::uint32_t scratch_liveness_conflicts = 0U;
};

struct X64FlowEdgeDiagnosticEdgeReport {
    std::uint64_t edge_id = 0U;
    std::uint32_t predecessor_block_id = 0U;
    std::uint32_t successor_block_id = 0U;
    bool g4_edge_audited = false;
    std::uint32_t g4_live_values = 0U;
    std::vector<X64FlowEdgeG4LiveValueDiagnosticReport> g4_values;
    std::uint32_t g6_candidate_values = 0U;
    std::vector<X64FlowEdgeG6CandidateDiagnosticReport> g6_candidates;
    bool g6_branch_anchor_present = false;
    bool g6_branch_anchor_identity_exact = false;
    std::uint8_t branch_anchor_encoding = 0U;
    std::uint8_t branch_edge_kind = 0U;
    bool branch_conditional_taken = false;
    std::uint32_t branch_instruction_offset = 0U;
    std::uint32_t branch_displacement_offset = 0U;
    std::uint32_t branch_target_offset = 0U;
    std::uint32_t branch_function_code_size = 0U;
    std::uint32_t g7_attempts = 0U;
    std::uint32_t g7_schedule_successes = 0U;
    std::uint32_t g7_schedule_rejections = 0U;
    std::vector<X64FlowEdgeG7ScratchTrialDiagnosticReport> g7_trials;
    bool g8_readiness_attempted = false;
    bool g8_readiness_audit_completed = false;
    std::string g8_rejection_code;
    std::string g8_rejection_message;
    std::uint32_t g8_live_values = 0U;
    std::uint32_t g8_endpoint_matches = 0U;
    std::uint32_t g8_endpoint_mismatches = 0U;
    bool g9_schedule_built = false;
    bool g9_scratch_selection_proven = false;
    std::uint8_t g9_selected_gp_temporary = 0U;
    std::uint8_t g9_selected_xmm_temporary = 0U;
    std::uint32_t g10_values_with_evidence = 0U;
    std::uint32_t g10_values_proven = 0U;
    std::uint32_t g10_values_mismatched = 0U;
    std::uint32_t g10_values_missing = 0U;
    NativeX64FlowOperandMismatchCounts g10_mismatch_counts;
    std::vector<X64FlowEdgeValueOperandEvidenceReport> g10_values;
    bool g11_exact_schedule = false;
    bool g11_endpoint_ownership_proven = false;
    bool g11_scratch_liveness_proven = false;
    bool g11_operation_operand_ownership_proven = false;
    bool g11_proof_gate_passed = false;
    std::vector<std::string> g11_blockers;
};

struct X64FlowEdgeDiagnosticFunctionReport {
    std::size_t function_index = static_cast<std::size_t>(-1);
    std::string function_name;
    bool g4_plan_succeeded = false;
    bool g6_plan_succeeded = false;
    bool g9_plan_succeeded = false;
    bool g10_plan_succeeded = false;
    bool g11_plan_succeeded = false;
    std::uint32_t g4_edges_audited = 0U;
    // Full G4 plan counter across its complete CFG edge universe.
    std::uint32_t g4_live_values_audited = 0U;
    // G4 records copied into this report's G6/G11 candidate-edge projection.
    std::uint32_t g4_candidate_edge_live_values = 0U;
    std::uint32_t g6_candidate_edges = 0U;
    std::uint32_t g6_candidate_values = 0U;
    std::uint32_t g7_attempts = 0U;
    std::uint32_t g7_schedule_successes = 0U;
    std::uint32_t g7_schedule_rejections = 0U;
    std::uint32_t g8_readiness_attempts = 0U;
    std::uint32_t g8_readiness_audit_completions = 0U;
    std::uint32_t edges_with_g6_branch_anchor_missing = 0U;
    std::uint32_t edges_with_g6_branch_anchor_inexact = 0U;
    std::vector<X64FlowEdgeDiagnosticEdgeReport> edges;
};

struct X64CodegenStats {
    std::uint32_t volatile_register_temporaries = 0U;
    std::uint32_t stack_spilled_temporaries = 0U;
    std::uint32_t stack_temporary_bytes_avoided = 0U;
    std::uint8_t maximum_volatile_register_index = 0U;
    std::uint32_t xmm_register_temporaries = 0U;
    std::uint32_t xmm_stack_spilled_temporaries = 0U;
    std::uint32_t local_stack_slots_reused = 0U;
    std::uint32_t local_stack_slots_peak = 0U;
    std::uint32_t stack_frame_bytes = 0U;
    std::uint32_t native_register_values_analyzed = 0U;
    std::uint32_t native_spill_slots = 0U;
    std::uint32_t callee_saved_gp_registers = 0U;
    std::uint32_t callee_saved_xmm_registers = 0U;
    std::uint32_t constant_folded_expressions = 0U;
    std::uint32_t peephole_optimizations = 0U;
    // N86-N88 flow-driven planning and object unwind emission accounting.
    std::uint32_t native_cfg_blocks_analyzed = 0U;
    std::uint32_t native_flow_values_analyzed = 0U;
    std::uint32_t native_flow_spill_actions_planned = 0U;
    std::uint32_t native_flow_spill_slots_planned = 0U;
    std::uint32_t win64_unwind_records_emitted = 0U;
    // N106: functions whose fixed Win64 stack frame is guarded by __chkstk.
    std::uint32_t win64_stack_probe_functions = 0U;
    // N90-N93 persistent source-variable locations and memory-SSA Phi coalescing.
    std::uint32_t native_persistent_homes_mapped = 0U;
    std::uint32_t native_call_crossing_values_homed = 0U;
    std::uint32_t native_home_phi_nodes = 0U;
    std::uint32_t native_home_phi_incoming_edges = 0U;
    std::uint32_t native_home_phi_edges_coalesced = 0U;
    std::uint32_t native_home_phi_critical_edges = 0U;
    std::uint32_t native_home_phi_machine_copies = 0U;
    // N96-N99 persistent source-variable values held in Win64 nonvolatile
    // registers while retaining canonical stack mirrors for address-taking.
    std::uint32_t native_persistent_gp_register_assignments = 0U;
    std::uint32_t native_persistent_xmm_register_assignments = 0U;
    std::uint32_t native_persistent_register_stack_fallbacks = 0U;
    std::uint32_t native_persistent_register_reads = 0U;
    std::uint32_t native_persistent_register_writes = 0U;
    std::uint32_t native_persistent_home_mirror_stores_elided = 0U;
    std::uint32_t native_address_taken_variables_kept_in_memory = 0U;
    // N108: source Flow operations mapped to final machine-code boundaries.
    std::uint32_t native_flow_emitter_anchors_emitted = 0U;
    // N109A: CFG control edges tied to final x64 branch/fallthrough locations.
    std::uint32_t native_cfg_branch_anchors_emitted = 0U;
    // N111: one reconciled active Machine Location per persistent Flow binding.
    std::uint32_t native_machine_location_plans_built = 0U;
    std::uint32_t native_machine_location_register_values = 0U;
    std::uint32_t native_machine_location_home_values = 0U;
    std::uint32_t native_machine_location_address_taken_values = 0U;
    // N113 focused proof mode; these counters remain zero unless explicitly enabled.
    std::uint32_t native_n113_flow_segment_plans_validated = 0U;
    std::uint32_t native_n113_flow_actions_resolved = 0U;
    std::uint32_t native_n113_actions_anchor_validated = 0U;
    std::uint32_t native_n113_actions_location_audited = 0U;
    std::uint32_t native_n113_action_values_without_persistent_location = 0U;
    std::uint32_t native_n113_action_location_register_matches = 0U;
    std::uint32_t native_n113_action_location_register_mismatches = 0U;
    std::uint32_t native_n113_action_location_unmapped_endpoints = 0U;
    std::uint32_t native_n113_action_location_unbound_endpoints = 0U;
    std::uint32_t native_n113_flow_operations_location_audited = 0U;
    std::uint32_t native_n113_flow_value_positions_location_audited = 0U;
    std::uint32_t native_n113_flow_positions_register_matches = 0U;
    std::uint32_t native_n113_flow_positions_register_mismatches = 0U;
    std::uint32_t native_n113_flow_positions_unmapped = 0U;
    std::uint32_t native_n113_flow_positions_unbound = 0U;
    std::uint32_t native_n113_flow_emitter_operations_mapped = 0U;
    std::uint32_t native_n113_flow_emitter_reads_mapped = 0U;
    std::uint32_t native_n113_flow_emitter_writes_mapped = 0U;
    std::uint32_t native_n113_flow_emitter_calls_mapped = 0U;
    std::uint32_t native_n113_flow_emitter_operand_matches = 0U;
    std::uint32_t native_n113_flow_emitter_operand_register_mismatches = 0U;
    std::uint32_t native_n113_flow_emitter_operand_unmapped = 0U;
    std::uint32_t native_n113_flow_frame_spill_slots_reserved = 0U;
    std::uint32_t native_n113_flow_frame_home_slots_reserved = 0U;
    // N113-G1 planner-only action endpoint rebinding and scratch proof.
    std::uint32_t native_n113_emitter_actions_rebound = 0U;
    std::uint32_t native_n113_emitter_memory_to_memory_actions = 0U;
    std::uint32_t native_n113_emitter_flow_spill_slots_rebased = 0U;
    std::uint32_t native_n113_emitter_flow_home_slots_rebased = 0U;
    std::uint32_t native_n113_emitter_gp_scratch_required = 0U;
    std::uint32_t native_n113_emitter_xmm_scratch_required = 0U;
    // N113-G2: real machine-code transfers inserted by the opt-in FunctionEmitter path.
    std::uint32_t native_n113_emitter_actions_inserted = 0U;
    std::uint32_t native_n113_emitter_machine_moves_inserted = 0U;
    std::uint32_t native_n113_emitter_transfer_bytes_inserted = 0U;
    // N113-G5: real-FunctionEmitter execution of the read-only G4 edge audit.
    std::uint32_t native_n113_flow_edge_audit_plans_built = 0U;
    std::uint32_t native_n113_flow_edges_audited = 0U;
    std::uint32_t native_n113_flow_edge_live_values_audited = 0U;
    std::uint32_t native_n113_flow_edge_location_changes = 0U;
    std::uint32_t native_n113_flow_edge_locations_coalesced = 0U;
    std::uint32_t native_n113_flow_home_phi_nodes_audited = 0U;
    std::uint32_t native_n113_flow_home_phi_incoming_edges_audited = 0U;
    // N113-G8: per-edge operand ownership and scratch-liveness evidence.
    std::uint32_t native_n113_flow_edge_schedule_plans_built = 0U;
    std::uint32_t native_n113_flow_edge_schedules_audited = 0U;
    std::uint32_t native_n113_flow_edge_operand_anchors_validated = 0U;
    std::uint32_t native_n113_flow_edge_endpoint_ownership_matches = 0U;
    std::uint32_t native_n113_flow_edge_endpoint_ownership_mismatches = 0U;
    std::uint32_t native_n113_flow_edge_gp_scratch_proofs = 0U;
    std::uint32_t native_n113_flow_edge_xmm_scratch_proofs = 0U;
    std::uint32_t native_n113_flow_edge_scratch_conflicts = 0U;
    std::uint32_t native_n113_flow_edge_scratch_schedule_unavailable = 0U;
    std::uint32_t native_n113_flow_edge_machine_ready_plans = 0U;
    // N113-G9: independently scheduled and scratch-proven exact CFG edges.
    std::uint32_t native_n113_flow_edge_per_edge_plans_built = 0U;
    std::uint32_t native_n113_flow_edge_per_edge_schedules_built = 0U;
    std::uint32_t native_n113_flow_edge_per_edge_scratch_proven = 0U;
    std::uint32_t native_n113_flow_edge_per_edge_scratch_blocked = 0U;
    std::uint32_t native_n113_flow_edge_per_edge_g7_rejections = 0U;
    std::uint32_t native_n113_flow_edge_per_edge_proof_rejections = 0U;
    std::string native_n113_flow_edge_per_edge_first_g7_rejection;
    std::string native_n113_flow_edge_per_edge_first_proof_rejection;
    std::uint32_t native_n113_flow_edge_per_edge_machine_ready_plans = 0U;
    // N113-G10: exact operation-level N112-to-FunctionEmitter operand evidence.
    std::uint32_t native_n113_flow_operation_operand_ownership_plans = 0U;
    std::uint32_t native_n113_flow_operation_operands_audited = 0U;
    std::uint32_t native_n113_flow_operation_operand_matches = 0U;
    std::uint32_t native_n113_flow_operation_operand_mismatches = 0U;
    std::uint32_t native_n113_flow_operation_address_operands_audited = 0U;
    // N113-G11: conservative per-edge conjunction of G8/G9/G10 evidence.
    std::uint32_t native_n113_flow_edge_bundle_eligibility_plans = 0U;
    std::uint32_t native_n113_flow_edge_bundle_candidate_edges = 0U;
    std::uint32_t native_n113_flow_edge_bundle_proof_gate_passed = 0U;
    std::uint32_t native_n113_flow_edge_bundle_blocked = 0U;
    std::uint32_t native_n113_flow_edge_bundle_schedule_blocked = 0U;
    std::uint32_t native_n113_flow_edge_bundle_endpoint_blocked = 0U;
    std::uint32_t native_n113_flow_edge_bundle_scratch_blocked = 0U;
    std::uint32_t native_n113_flow_edge_bundle_operand_blocked = 0U;
    // N113-G12: deterministic per-function/per-edge reports; empty unless G11 is enabled.
    std::vector<X64FlowEdgeBundleEligibilityFunctionReport>
        native_n113_flow_edge_bundle_function_reports;
    // N113-G20: parallel diagnostic view; never used by G11 proof decisions.
    std::vector<X64FlowEdgeDiagnosticFunctionReport>
        native_n113_flow_edge_diagnostic_function_reports;
    std::vector<X64FlowFrameScratchDiagnosticFunctionReport>
        native_n113_frame_scratch_diagnostic_function_reports;
};

struct X64FlowEmitterAnchor {
    std::size_t function_index = InvalidNativeFunctionIndex;
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    NativeRegisterBlockId flow_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t flow_operation_index = InvalidNativeFlowOperationIndex;
    NativeRegisterInstructionOriginKind origin_kind = NativeRegisterInstructionOriginKind::Synthetic;
    SourceSpan source_span;
    // Primary operation boundary (after the read/definition/call sequence).
    std::uint32_t machine_code_offset = 0U;
    // Optional pre-operation boundary. For calls this is before argument
    // evaluation, so call-crossing stores can be placed before any clobber.
    std::uint32_t machine_code_before_offset = InvalidNativeFlowOperationIndex;
    // Optional boundary immediately after the CALL instruction and before
    // caller stack cleanup; used for call-crossing reloads.
    std::uint32_t machine_code_after_call_offset = InvalidNativeFlowOperationIndex;
    std::uint32_t function_code_size = 0U;
    // Exact CALL rel32 instruction start when the operation is FlowCall.
    std::uint32_t machine_code_call_instruction_offset = InvalidNativeFlowOperationIndex;
    // The N111 active location actually selected by the emitter for a mapped
    // source-value read/write at this operation. AddressRead refers to the
    // canonical home address rather than loading the value stored there.
    enum class OperandKind : std::uint8_t {
        None,
        ValueRead,
        AddressRead,
        ValueWrite,
        ValueDeclare
    } emitted_operand_kind = OperandKind::None;
    std::uint32_t emitted_operand_value_id = 0U;
    NativeRegisterLocation emitted_operand_location{};
};


enum class X64FlowBranchEncoding : std::uint8_t {
    Fallthrough,
    ConditionalRel32,
    UnconditionalRel32
};

struct X64FlowBranchAnchor {
    std::size_t function_index = InvalidNativeFunctionIndex;
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    FlowControlStatementId control_statement_id = InvalidFlowControlStatementId;
    FlowControlConstructId control_construct_id = InvalidFlowControlConstructId;
    FlowBlockId from_block_id = InvalidFlowBlockId;
    FlowBlockId to_block_id = InvalidFlowBlockId;
    FlowEdgeKind edge_kind = FlowEdgeKind::Sequential;
    X64FlowBranchEncoding encoding = X64FlowBranchEncoding::Fallthrough;
    // For conditional branches, true means this CFG edge is the taken path;
    // false means it is the fallthrough path of the same Jcc instruction.
    bool conditional_taken = false;
    std::uint32_t instruction_offset = 0U;
    std::uint32_t displacement_offset = InvalidNativeFlowOperationIndex;
    std::uint32_t target_offset = 0U;
    std::uint32_t function_code_size = 0U;
    // N109B: optional physical edge-transfer island/redirect metadata.
    std::uint32_t transfer_instruction_offset = InvalidNativeFlowOperationIndex;
    std::uint32_t transfer_displacement_offset = InvalidNativeFlowOperationIndex;
    std::uint32_t transfer_target_offset = InvalidNativeFlowOperationIndex;
    std::uint32_t transfer_byte_count = 0U;
};

enum class X64ExpressionBranchEncoding : std::uint8_t {
    Fallthrough,
    ConditionalRel32,
    UnconditionalRel32
};

// Expression-level machine branch evidence is deliberately separate from
// X64FlowBranchAnchor: these edges are not N113 statement/loop edge-bundle
// candidates. Offsets are function-local and rebased after every code insertion.
struct X64ExpressionBranchAnchor {
    std::size_t function_index = InvalidNativeFunctionIndex;
    FlowExpressionId expression_id = InvalidFlowExpressionId;
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    FlowBlockId from_block_id = InvalidFlowBlockId;
    FlowBlockId to_block_id = InvalidFlowBlockId;
    FlowEdgeKind edge_kind = FlowEdgeKind::Sequential;
    X64ExpressionBranchEncoding encoding = X64ExpressionBranchEncoding::Fallthrough;
    bool conditional_taken = false;
    std::uint32_t instruction_offset = 0U;
    std::uint32_t displacement_offset = InvalidNativeFlowOperationIndex;
    std::uint32_t target_offset = 0U;
    std::uint32_t source_block_begin_offset = 0U;
    std::uint32_t source_block_end_offset = 0U;
    std::uint32_t function_code_size = 0U;
};

// Production occupancy evidence is deliberately independent of branch anchors
// and N113 Statement-edge schedules. A capture offset is the boundary BEFORE
// bytes inserted at the same offset.
enum class X64ExpressionEdgeCaptureSite : std::uint8_t {
    AfterConditionalJccBeforeRhs,
    AfterRhsNormalizeBeforeMergeJump,
    AfterShortResultBeforeFallthrough
};

struct X64ExpressionEdgeOccupancyCapture {
    std::size_t function_index = InvalidNativeFunctionIndex;
    FlowExpressionId expression_id = InvalidFlowExpressionId;
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    FlowBlockId from_block_id = InvalidFlowBlockId;
    FlowBlockId to_block_id = InvalidFlowBlockId;
    FlowEdgeKind edge_kind = FlowEdgeKind::Sequential;
    X64ExpressionEdgeCaptureSite capture_site = X64ExpressionEdgeCaptureSite::AfterConditionalJccBeforeRhs;
    std::uint32_t original_code_offset = 0U;
    std::uint32_t code_offset = 0U;
    std::vector<std::uint8_t> gp_stack_order;
    std::vector<std::uint8_t> xmm_stack_order;
    std::vector<std::uint8_t> gp_occupancy;
    std::vector<std::uint8_t> xmm_occupancy;
    std::uint32_t gp_depth = 0U;
    std::uint32_t xmm_depth = 0U;
    bool capture_complete = false;
};

struct X64ExpressionEdgeOccupancyOffsetRebaseResult {
    bool succeeded = false;
    std::uint32_t mapped_offset = 0U;
    std::string diagnostic;
};

// Maps a pre-insertion boundary with BEFORE affinity. Adjustments must be
// strictly increasing, nonzero, within source_code_size, and compose exactly
// to final_code_size. Equality does not move the captured boundary.
X64ExpressionEdgeOccupancyOffsetRebaseResult rebase_x64_expression_edge_occupancy_offset_before(
    std::uint32_t source_offset, std::uint32_t source_code_size,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& adjustments,
    std::uint32_t final_code_size);

struct X64CodegenResult {
    NativeObjectEmissionResult native_object;
    std::vector<std::uint8_t> object_bytes;
    std::vector<Diagnostic> diagnostics;
    X64CodegenStats stats;
    // Deterministic side-table; not serialized into COFF object sections.
    std::vector<X64FlowEmitterAnchor> flow_anchors;
    // Deterministic side-table for CFG control-flow edges; not serialized.
    std::vector<X64FlowBranchAnchor> flow_branch_anchors;
    // Separate identity/offset proof for Expression Micro-CFG edges; never
    // forwarded into N113's statement-edge transfer schedule.
    std::vector<X64ExpressionBranchAnchor> flow_expression_branch_anchors;
    // Independent per-Expression-FlowEdge occupancy snapshots from the three
    // actual emission seams; offsets are final function-local BEFORE boundaries.
    std::vector<X64ExpressionEdgeOccupancyCapture> flow_expression_edge_occupancy_captures;

    bool succeeded() const noexcept { return diagnostics.empty() && !object_bytes.empty(); }
};

struct X64ExpressionBranchAnchorValidationResult {
    bool succeeded = false;
    std::vector<std::string> diagnostics;
};

X64ExpressionBranchAnchorValidationResult validate_x64_expression_branch_anchors(
    const std::vector<std::uint8_t>& function_code,
    const std::vector<FlowExpressionControl>& expression_controls,
    const std::vector<FlowEdge>& edges,
    const std::vector<X64ExpressionBranchAnchor>& anchors,
    std::size_t function_index);

struct X64ExpressionEdgeOccupancyValidationResult {
    bool succeeded = false;
    std::vector<std::string> diagnostics;
};

X64ExpressionEdgeOccupancyValidationResult validate_x64_expression_edge_occupancy_captures(
    const std::vector<std::uint8_t>& function_code,
    const std::vector<FlowExpressionControl>& expression_controls,
    const std::vector<FlowEdge>& edges,
    const std::vector<X64ExpressionBranchAnchor>& branch_anchors,
    const std::vector<X64ExpressionEdgeOccupancyCapture>& captures,
    std::size_t function_index);

// Win64 AMD64 backend for the supported scalar subset. It lowers integer,
// boolean, float/double, local-pointer and direct-call expressions to x86-64
// machine code. N54 introduced expression-temporary linear scan; N72-N83 add
// expression and CFG-derived persistent-value allocation plans. N84-N89 emit
// per-function Win64 UNWIND_INFO in associative COFF .pdata/.xdata sections.
// N90-N95 bind scalar source variables to canonical stack homes and coalesce
// memory-SSA Phi inputs on those homes. N96-N101 additionally allocate eligible
// persistent scalar locals/parameters to Win64 nonvolatile GP/XMM registers,
// mirror writes to canonical homes, keep address-taken values memory-resident,
// and emit matching save/restore plus UNWIND_INFO actions. Values that exceed
// the available nonvolatile register pools safely remain in their stack homes.
X64CodegenResult compile_x64_coff_object(const CompilationUnit& unit,
                                         X64CodegenOptions options = {});

} // namespace xfrontend
