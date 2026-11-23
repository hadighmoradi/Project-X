#pragma once

#include "xfrontend/native_cfg_bridge.hpp"
#include "xfrontend/x64_codegen.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xfrontend {

// RBP-relative locations used by the Win64 frame planner. Values must be
// negative, 8-byte aligned displacements within signed disp32 range.
struct NativeX64FrameLocationMap {
    std::map<std::uint32_t, std::int32_t> spill_slot_offsets;
    std::map<std::uint32_t, std::int32_t> home_slot_offsets;
};

// N113-F: reserve the allocator's abstract Flow spill/home slots in the
// FunctionEmitter Win64 frame without inserting any transfers. Flow slot IDs
// are mapped after the emitter's existing spill/home ranges, keeping both
// namespaces disjoint while preserving the base frame's existing offsets.
struct NativeX64FlowFrameBindingPlan {
    bool succeeded = false;
    NativeWin64FramePlan frame_plan;
    NativeX64FrameLocationMap flow_frame_locations;
    std::uint32_t flow_spill_slots_reserved = 0U;
    std::uint32_t flow_home_slots_reserved = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeX64FlowFrameBindingPlan plan_native_x64_flow_frame_binding(
    const NativeRegisterAllocationPlan& emitter_allocation,
    const NativeRegisterAllocationPlan& flow_allocation,
    std::uint32_t local_area_bytes);

// N111: one authoritative active Machine Location for every scalar/pointer
// Flow binding. The canonical home is kept separately for addressability,
// memory-SSA Phi coalescing and safe stack fallback. Home offsets are assigned
// later by the Win64 frame planner; InvalidNativeX64HomeSlot is a planning-only
// sentinel until FunctionEmitter binds the lexical local slot.
constexpr std::uint32_t InvalidNativeX64HomeSlot = static_cast<std::uint32_t>(-1);

struct NativeX64MachineValueLocation {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterLocation active_location;
    NativeRegisterLocation canonical_home;
    bool address_taken = false;
};

struct NativeX64MachineLocationPlan {
    bool succeeded = false;
    std::map<std::uint32_t, NativeX64MachineValueLocation> values;
    std::vector<std::uint8_t> used_callee_saved_gp;
    std::vector<std::uint8_t> used_callee_saved_xmm;
    std::uint32_t register_resident_values = 0U;
    std::uint32_t home_resident_values = 0U;
    std::uint32_t address_taken_values = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Reconciles the persistent-value allocation that FunctionEmitter actually
// consumes with the Flow CFG and expression-temporary register reservations.
// Address-taken or spilled bindings remain in their canonical HomeSlot. The
// result is rejected if classes, Win64 nonvolatile rules, reserved registers,
// binding identity, or CFG interference constraints are inconsistent.
NativeX64MachineLocationPlan build_native_x64_machine_location_plan(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterAllocationResult& persistent_allocation,
    const std::set<std::uint32_t>& address_taken_value_ids,
    const std::vector<std::uint8_t>& reserved_expression_gp,
    const std::vector<std::uint8_t>& reserved_expression_xmm);

// Binds the plan's unresolved canonical HomeSlot sentinel to the concrete
// FunctionEmitter frame slot. Home-resident values also make that concrete
// home their active location; register-resident values retain their register.
// Rebinding the same value to the same slot is idempotent.
bool bind_native_x64_machine_location_home(
    NativeX64MachineLocationPlan& plan,
    std::uint32_t value_id,
    std::uint32_t home_slot);

struct NativeX64ParallelCopyEncodingResult {
    bool succeeded = false;
    std::vector<std::uint8_t> bytes;
    std::uint32_t machine_moves_emitted = 0U;
    std::uint32_t memory_to_memory_expansions = 0U;
    std::uint32_t temporary_register_moves = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Encodes an already-resolved simultaneous-copy schedule as real x64 MOV
// instructions. GP values use 64-bit MOV; XMM values use MOVQ so the 8-byte
// Win64 scalar home/spill slot contract is preserved. Memory-to-memory moves
// expand through the caller-reserved temporary register. This API emits bytes
// only; placing edge bundles into a function body is a separate integration
// stage and is not implied by this encoder.
NativeX64ParallelCopyEncodingResult encode_native_x64_parallel_copy_schedule(
    const std::vector<NativeRegisterParallelCopyOperation>& operations,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation cycle_temporary,
    const NativeX64FrameLocationMap& frame_locations);

// N113-G22: encodes a schedule whose cycle storage is an explicitly mapped,
// dedicated SpillSlot. A separate class-appropriate Temporary register is
// reserved only for memory-to-memory expansion. The SpillSlot must be distinct
// from every other physical frame-map offset, and the expansion register must
// not appear as a source/destination operand in the schedule. The caller must
// separately prove the SpillSlot is dead/free at both exact edge endpoints (G4),
// the expansion register is safe for the edge (G9 plus ABI save/restore rules),
// and the supplied frame map is part of an already-reserved active frame. This
// API only returns bytes; it does not reserve a frame slot or insert edge code.
NativeX64ParallelCopyEncodingResult encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
    const std::vector<NativeRegisterParallelCopyOperation>& operations,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation memory_cycle_temporary,
    NativeRegisterLocation expansion_temporary,
    const NativeX64FrameLocationMap& frame_locations);

// N113-G24: GP-only memory-cycle schedules may use a separate volatile XMM1-XMM5
// register for bit-preserving MOVQ memory-to-memory expansion. The XMM register
// bank is distinct from the GP bank even where the numeric indices match. The
// caller must still prove exact-edge XMM deadness (G9), dedicated frame-slot
// availability (G4), and ABI safety. This API emits bytes only; it performs no
// frame reservation, unwind update, or edge insertion.
NativeX64ParallelCopyEncodingResult encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
    const std::vector<NativeRegisterParallelCopyOperation>& operations,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation memory_cycle_temporary,
    NativeRegisterLocation xmm_expansion_temporary,
    const NativeX64FrameLocationMap& frame_locations);

struct NativeX64FlowEdgeLocationAuditPlan;
struct NativeX64FlowEdgeTransferCandidatePlan;

// N113-G25: exact-edge audit of the Win64 volatile XMM1-XMM5 pool for GP
// memory-to-memory expansion. G4 live-through sets are independently reconciled
// against predecessor.live_out intersect successor.live_in. Every FP live value
// is checked at both N112 endpoints and its authoritative N111 active location;
// GP numeric register indices are intentionally a different physical bank.
// This is a read-only availability proof only: it does not prove a cycle
// SpillSlot is reserved, does not insert code, and never marks insertion ready.
struct NativeX64FlowEdgeXmmExpansionRegisterEvidence {
    std::uint8_t xmm_register = 0U;
    bool win64_volatile = false;
    bool g4_live_set_complete = false;
    bool expression_reserved = false;
    bool live_value_conflict = false;
    bool available = false;
    std::uint32_t conflicting_values = 0U;
    std::vector<std::uint32_t> conflicting_value_ids;
};

struct NativeX64FlowEdgeGpXmmExpansionEvidence {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId successor_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t expected_g4_live_values = 0U;
    std::uint32_t actual_g4_live_values = 0U;
    std::uint32_t g6_candidate_values = 0U;
    bool g4_live_set_complete = false;
    bool branch_anchor_identity_exact = false;
    bool gp_only_candidates = false;
    bool gp_memory_to_memory_required = false;
    bool xmm_expansion_selection_proven = false;
    std::uint8_t selected_xmm_register = 0U;
    std::vector<NativeX64FlowEdgeXmmExpansionRegisterEvidence> xmm_candidates;
};

struct NativeX64GpXmmExpansionLivenessPlan {
    bool succeeded = false;
    bool g4_live_sets_reconciled = false;
    bool candidate_edge_identities_exact = false;
    bool machine_insertion_ready = false;
    std::uint32_t cfg_edges_reconciled = 0U;
    std::uint32_t candidate_edges_audited = 0U;
    std::uint32_t g4_live_values_reconciled = 0U;
    std::uint32_t gp_memory_expansion_edges = 0U;
    std::uint32_t xmm_register_candidates_audited = 0U;
    std::uint32_t xmm_register_candidates_dead = 0U;
    std::uint32_t xmm_register_candidates_blocked = 0U;
    std::uint32_t edges_with_dead_expansion_register = 0U;
    bool all_required_edges_have_dead_xmm = false;
    std::vector<NativeX64FlowEdgeGpXmmExpansionEvidence> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeX64GpXmmExpansionLivenessPlan audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeX64FlowEdgeLocationAuditPlan& edge_location_audit,
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const std::vector<std::uint8_t>& expression_reserved_xmm);

// N113-G26: ordered, multi-edge protocol audit for reusing one dedicated
// memory cycle SpillSlot across edge-local GP schedules. Within each exact
// edge, every cycle transaction must be strictly Save -> (zero or more
// non-cycle moves) -> Restore for the same ValueId. A second Save while the
// slot is occupied, a Restore without a preceding Save, a mismatched ValueId,
// or an unclosed transaction is rejected. Across CFG edges the same slot and
// already-proven XMM1-XMM5 register may be reused because each bundle closes
// its cycle-storage lifetime before its edge completes. This is an audit only:
// it does not reserve a frame slot, mutate unwind data, or insert code.
struct NativeX64GpMemoryCycleEdgeScheduleInput {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    std::vector<NativeRegisterParallelCopyOperation> operations;
};

struct NativeX64MemoryCycleTransactionEvidence {
    std::uint32_t cycle_index = 0U;
    std::uint32_t value_id = 0U;
    std::uint32_t save_operation_index = 0U;
    std::uint32_t restore_operation_index = 0U;
};

struct NativeX64MultiEdgeMemoryCycleEdgeEvidence {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    std::uint8_t selected_xmm_register = 0U;
    std::uint32_t operations_audited = 0U;
    std::uint32_t cycle_transactions_audited = 0U;
    bool g25_exact_edge_xmm_proof_matched = false;
    bool ordered_cycle_protocol_valid = false;
    std::vector<NativeX64MemoryCycleTransactionEvidence> transactions;
};

struct NativeX64MultiEdgeMemoryCycleAuditPlan {
    bool succeeded = false;
    bool g25_edge_coverage_exact = false;
    bool ordered_cycle_protocol_valid = false;
    bool cycle_slot_frame_mapping_valid = false;
    bool cycle_slot_reused_across_edges = false;
    bool cycle_slot_reused_across_transactions = false;
    bool machine_insertion_ready = false;
    std::uint32_t edges_audited = 0U;
    std::uint32_t total_operations_audited = 0U;
    std::uint32_t total_cycle_transactions_audited = 0U;
    std::uint32_t distinct_xmm_registers_selected = 0U;
    std::vector<NativeX64MultiEdgeMemoryCycleEdgeEvidence> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeX64MultiEdgeMemoryCycleAuditPlan audit_native_x64_multi_edge_gp_memory_cycle_protocol(
    const NativeX64GpXmmExpansionLivenessPlan& g25_liveness,
    const std::vector<NativeX64GpMemoryCycleEdgeScheduleInput>& edge_schedules,
    NativeRegisterLocation cycle_spill_slot,
    const NativeX64FrameLocationMap& frame_locations);

// N113-G4: read-only crosswalk of block-boundary Flow allocations to the
// authoritative locations that FunctionEmitter actually consumes. N112 may
// assign different locations to the same value in adjacent blocks; those
// differences are recorded as pending Flow-allocation transitions, never
// emitted as machine copies here. Memory-SSA Phi aliases are validated against
// the canonical N111 homes. Physical edge insertion remains a later phase.
struct NativeX64FlowEdgeValueAudit {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId successor_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterLocation predecessor_flow_location;
    NativeRegisterLocation successor_flow_location;
    NativeRegisterLocation emitter_active_location;
    std::int32_t predecessor_frame_displacement = 0;
    std::int32_t successor_frame_displacement = 0;
    bool predecessor_uses_frame = false;
    bool successor_uses_frame = false;
    bool flow_location_changes = false;
};

struct NativeX64FlowEdgeLocationAuditPlan {
    bool succeeded = false;
    std::uint32_t edges_audited = 0U;
    std::uint32_t live_values_audited = 0U;
    std::uint32_t flow_location_changes = 0U;
    std::uint32_t flow_locations_coalesced = 0U;
    std::uint32_t home_phi_nodes_audited = 0U;
    std::uint32_t home_phi_incoming_edges_audited = 0U;
    std::vector<NativeX64FlowEdgeValueAudit> values;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Audits exact N112 segment locations at predecessor-exit/successor-entry
// positions for every value live across a real CFG edge. It checks N111's
// authoritative Emitter location independently and verifies Flow frame
// displacements. It also validates that current memory-SSA Phi aliases remain
// coalesced on the same source home. This API is intentionally audit-only: it
// does not create edge bundles or modify machine code/anchors/relocations.
NativeX64FlowEdgeLocationAuditPlan audit_native_x64_flow_edge_locations(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals,
    const NativeRegisterFlowSegmentPlan& flow_segments,
    const NativeRegisterHomePhiPlan& home_phi_plan,
    const NativeX64MachineLocationPlan& emitter_locations,
    const NativeX64FrameLocationMap& flow_frame_locations);

// N113-G6: validated, unscheduled candidates for location transitions on exact
// CFG edges. These are derived from N112's abstract Flow locations, not from
// machine operands consumed by FunctionEmitter. The N111 emitter location is
// retained as a separate authority check. Consequently this planner never
// declares candidates ready for code insertion; a later phase must prove that
// N112 edge locations are the actual emitted operand locations before calling
// the N109B edge-bundle insertion API.
struct NativeX64FlowEdgeTransferCandidate {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId successor_block_id = InvalidNativeRegisterBlockId;
    X64FlowBranchAnchor branch_anchor;
    NativeRegisterParallelCopy copy;
    NativeRegisterLocation emitter_active_location;
};

struct NativeX64FlowEdgeTransferCandidatePlan {
    bool succeeded = false;
    bool ready_for_machine_insertion = false;
    std::uint32_t flow_location_changes_considered = 0U;
    std::uint32_t candidate_copies = 0U;
    std::uint32_t candidate_edges = 0U;
    std::vector<NativeX64FlowEdgeTransferCandidate> candidates;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeX64FlowEdgeTransferCandidatePlan plan_native_x64_flow_edge_transfer_candidates(
    const NativeX64FlowEdgeLocationAuditPlan& audit,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes);


// Relative branches emitted by FunctionEmitter. The target is kept separately
// from encoded bytes so code insertions can safely rebase and repatch every
// internal rel32 branch (including short-circuit and epilogue branches).
struct NativeX64RelativeBranchFixup {
    std::uint32_t displacement_offset = 0U;
    std::uint32_t target_offset = 0U;
};

struct NativeX64CodeInsertionAdjustment {
    std::uint32_t original_offset = 0U;
    std::uint32_t inserted_bytes = 0U;
};

struct NativeX64EdgeTransferBundle {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    std::vector<NativeRegisterParallelCopyOperation> operations;
};

// N113-G7: deterministic per-edge schedule of the G6 candidates. The
// schedule is still planning-only: it does not prove that N112 endpoints are
// currently materialized as FunctionEmitter machine operands, and it does not
// prove cycle-temporary liveness outside the candidate bundle. Therefore
// machine_insertion_ready and scratch_liveness_proven remain false.
struct NativeX64FlowEdgeTransferSchedule {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId successor_block_id = InvalidNativeRegisterBlockId;
    X64FlowBranchAnchor branch_anchor;
    NativeX64EdgeTransferBundle bundle;
    std::uint32_t gp_cycle_temporaries_used = 0U;
    std::uint32_t xmm_cycle_temporaries_used = 0U;
};

struct NativeX64FlowEdgeTransferSchedulePlan {
    bool succeeded = false;
    bool machine_insertion_ready = false;
    bool scratch_liveness_proven = false;
    std::uint32_t candidate_edges = 0U;
    std::uint32_t candidate_copies = 0U;
    std::uint32_t scheduled_edges = 0U;
    std::uint32_t scheduled_copies = 0U;
    std::uint32_t scheduled_operations = 0U;
    std::uint32_t gp_cycle_temporaries_used = 0U;
    std::uint32_t xmm_cycle_temporaries_used = 0U;
    std::vector<NativeX64FlowEdgeTransferSchedule> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Resolves each G6 CFG-edge candidate set as a simultaneous copy, grouped by
// exact FlowEdgeId and GP/XMM class. The caller supplies dedicated caller-saved
// temporaries in the established R8-R11 and XMM1-XMM5 pools. The underlying
// resolver rejects aliasing temporaries and conflicting destinations. This
// stage never calls edge insertion and never declares the result insertion-ready.
NativeX64FlowEdgeTransferSchedulePlan schedule_native_x64_flow_edge_transfer_candidates(
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation gp_cycle_temporary,
    NativeRegisterLocation xmm_cycle_temporary);

// N113-G19: per-live-Value endpoint evidence captured at the exact point where
// G8 compares N112 predecessor/successor locations with the N111 Emitter value.
// These records are diagnostic only and do not participate in readiness logic.
struct NativeX64FlowEdgeEndpointValueEvidence {
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

// N113-G8: readiness evidence for one exact CFG edge. This is still an audit
// result; it never mutates code or branch anchors. Operand ownership is proven
// only when both N112 endpoints resolve to the concrete operand location used
// by FunctionEmitter (N111), including physical frame displacement equality.
struct NativeX64FlowEdgeReadinessEntry {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId successor_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t live_values_audited = 0U;
    std::uint32_t operand_anchors_validated = 0U;
    std::uint32_t endpoint_ownership_matches = 0U;
    std::uint32_t endpoint_ownership_mismatches = 0U;
    bool gp_scratch_required = false;
    bool xmm_scratch_required = false;
    bool gp_scratch_dead = true;
    bool xmm_scratch_dead = true;
    std::uint8_t gp_scratch_register = 0U;
    std::uint8_t xmm_scratch_register = 0U;
    std::vector<NativeX64FlowEdgeEndpointValueEvidence> endpoint_value_evidence;
    bool machine_operand_ownership_proven = false;
    bool scratch_liveness_proven = false;
};

struct NativeX64FlowEdgeScheduleReadinessPlan {
    bool succeeded = false;
    bool machine_operand_ownership_proven = false;
    bool scratch_liveness_proven = false;
    bool machine_insertion_ready = false;
    std::uint32_t scheduled_edges_audited = 0U;
    std::uint32_t live_edge_values_audited = 0U;
    std::uint32_t operand_anchors_validated = 0U;
    std::uint32_t endpoint_ownership_matches = 0U;
    std::uint32_t endpoint_ownership_mismatches = 0U;
    std::uint32_t gp_scratch_registers_proven_dead = 0U;
    std::uint32_t xmm_scratch_registers_proven_dead = 0U;
    std::uint32_t scratch_liveness_conflicts = 0U;
    std::vector<NativeX64FlowEdgeReadinessEntry> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Re-checks G7 schedules against the actual FunctionEmitter operand anchors,
// N111 persistent locations and concrete frame offsets. Scratch safety is
// evaluated per exact edge against N112 live-through values, N111 live
// locations and the caller-supplied expression-temporary reservation sets.
// A successful audit may still report insertion_ready=false when either proof
// is incomplete. No machine-code insertion or metadata mutation is performed.
NativeX64FlowEdgeScheduleReadinessPlan audit_native_x64_flow_edge_schedule_readiness(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeX64FlowEdgeLocationAuditPlan& edge_location_audit,
    const NativeX64FlowEdgeTransferSchedulePlan& schedule,
    const NativeX64MachineLocationPlan& emitter_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    NativeRegisterLocation gp_cycle_temporary,
    NativeRegisterLocation xmm_cycle_temporary,
    const std::vector<std::uint8_t>& expression_reserved_gp,
    const std::vector<std::uint8_t>& expression_reserved_xmm);

// N113-G20: one diagnostic record for each deterministic GP/XMM scratch-pair
// attempt made by G9. These records explain G7/G8 rejection paths and do not
// participate in schedule selection or readiness decisions.
struct NativeX64FlowEdgeG7ScratchTrialEvidence {
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

// N113-G9: each exact CFG edge receives its own pair of cycle/memory-expansion
// temporaries. This removes G8's unnecessary requirement that one GP/XMM pair
// be dead on every edge in the function. It remains an audit/planning result:
// edge schedules are not insertion-ready until the corresponding N112
// endpoints are proven to be the concrete FunctionEmitter operands.
struct NativeX64FlowEdgePerEdgeScratchEntry {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    bool schedule_built = false;
    bool scratch_selection_proven = false;
    NativeRegisterLocation gp_cycle_temporary{};
    NativeRegisterLocation xmm_cycle_temporary{};
    NativeX64FlowEdgeTransferSchedule schedule;
    NativeX64FlowEdgeReadinessEntry readiness;
    // G20 diagnostic snapshots. The G4/G6 records and G7 trials are copied
    // without changing the established G9 selection order or proof booleans.
    std::vector<NativeX64FlowEdgeValueAudit> g4_live_through_values;
    std::vector<NativeX64FlowEdgeTransferCandidate> g6_candidates;
    X64FlowBranchAnchor g6_branch_anchor;
    bool g6_branch_anchor_present = false;
    bool g6_branch_anchor_identity_exact = false;
    std::vector<NativeX64FlowEdgeG7ScratchTrialEvidence> g7_trials;
    bool g8_readiness_attempted = false;
    bool g8_readiness_audit_completed = false;
    std::string g8_rejection_code;
    std::string g8_rejection_message;
};

struct NativeX64FlowEdgePerEdgeScratchPlan {
    bool succeeded = false;
    bool machine_operand_ownership_proven = false;
    bool scratch_liveness_proven = false;
    bool machine_insertion_ready = false;
    std::uint32_t candidate_edges = 0U;
    std::uint32_t scheduled_edges = 0U;
    std::uint32_t scratch_proven_edges = 0U;
    std::uint32_t scratch_unavailable_edges = 0U;
    std::uint32_t g7_schedule_rejections = 0U;
    std::uint32_t readiness_proof_rejections = 0U;
    std::uint32_t endpoint_ownership_mismatches = 0U;
    std::uint32_t scratch_liveness_conflicts = 0U;
    std::string first_g7_rejection;
    std::string first_readiness_proof_rejection;
    std::vector<NativeX64FlowEdgePerEdgeScratchEntry> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeX64FlowEdgePerEdgeScratchPlan plan_native_x64_flow_edge_schedules_per_edge(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeX64FlowEdgeLocationAuditPlan& edge_location_audit,
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const NativeX64MachineLocationPlan& emitter_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::vector<std::uint8_t>& expression_reserved_gp,
    const std::vector<std::uint8_t>& expression_reserved_xmm);

// N113-G10: exact operation-level crosswalk from the N112 before/after
// location consumed by a source Flow operand to the concrete N111 operand
// emitted by FunctionEmitter. Mismatches are retained as structured evidence
// (not fatal to normal code generation); missing/ambiguous identities are
// structural errors. This API is read-only and never rewrites operands/code.
// N113-G16: records which side of the N112 operation supplied the audited
// location. Reads/address reads consume the before-position; writes/declarations
// consume the after-position.
struct NativeX64FlowOperationOperandOwnershipEntry {
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    NativeRegisterBlockId flow_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t flow_operation_index = InvalidNativeFlowOperationIndex;
    std::uint32_t value_id = 0U;
    // Actual N112 Flow-operation class and authoritative N111 Value class are
    // separate; G15 retains mismatches instead of erasing their evidence.
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterClass candidate_value_register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterInstructionOriginKind flow_origin_kind = NativeRegisterInstructionOriginKind::Synthetic;
    NativeX64FlowOperandPositionKind flow_position_kind = NativeX64FlowOperandPositionKind::BeforeOperation;
    SourceSpan source_span{};
    bool source_span_matches_emitter_anchor = false;
    X64FlowEmitterAnchor::OperandKind operand_kind = X64FlowEmitterAnchor::OperandKind::None;
    NativeRegisterLocation flow_operation_location;
    NativeRegisterLocation emitter_operand_location;
    bool flow_location_uses_frame = false;
    std::int32_t flow_frame_displacement = 0;
    bool emitter_location_uses_frame = false;
    std::int32_t emitter_frame_displacement = 0;
    bool location_matches = false;
};

struct NativeX64FlowOperationOperandOwnershipPlan {
    bool succeeded = false;
    bool all_operands_owned = false;
    std::size_t function_index = InvalidNativeFunctionIndex;
    std::uint32_t source_operations_audited = 0U;
    std::uint32_t operand_operations_audited = 0U;
    std::uint32_t operand_location_matches = 0U;
    std::uint32_t operand_location_mismatches = 0U;
    std::uint32_t address_operands_audited = 0U;
    std::uint32_t frame_location_matches = 0U;
    std::uint32_t register_location_matches = 0U;
    std::vector<NativeX64FlowOperationOperandOwnershipEntry> operands;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeX64FlowOperationOperandOwnershipPlan audit_native_x64_flow_operation_operand_ownership(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterFlowSegmentPlan& flow_segment_plan,
    const NativeX64MachineLocationPlan& emitter_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    std::size_t function_index,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors);

// N113-G11: read-only intersection of the G6 exact-edge candidate inventory,
// G9 per-edge scratch/liveness proof and G10 operation-level operand ownership.
// A passed proof_gate means that this edge has enough evidence to be considered
// by a later insertion stage; it does NOT mean code insertion is performed or
// that rel32/COFF relocation/UNWIND integration has been validated.
struct NativeX64FlowEdgeBundleOperandEvidenceEntry {
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    NativeRegisterBlockId flow_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t flow_operation_index = InvalidNativeFlowOperationIndex;
    std::uint8_t operand_kind = 0U;
    // G15/G16: preserve both classes and the source operation context so a
    // mismatch can be traced to its Flow role, N112 position and source span.
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

struct NativeX64FlowEdgeValueOperandEvidenceEntry {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    bool concrete_evidence_found = false;
    std::uint32_t operand_operations = 0U;
    std::uint32_t matching_operations = 0U;
    std::uint32_t mismatching_operations = 0U;
    bool all_observed_operands_match = false;
    NativeX64FlowOperandMismatchCounts mismatch_counts;
    std::vector<NativeX64FlowEdgeBundleOperandEvidenceEntry> operations;
};

struct NativeX64FlowEdgeBundleEligibilityEntry {
    FlowEdgeId edge_id = InvalidFlowEdgeId;
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId successor_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t candidate_values = 0U;
    // Counts candidate ValueIds for which every observed G10 operand matches.
    std::uint32_t operation_values_proven = 0U;
    std::uint32_t operation_values_with_evidence = 0U;
    std::uint32_t operation_values_mismatched = 0U;
    std::uint32_t operation_values_missing = 0U;
    NativeX64FlowOperandMismatchCounts mismatch_counts;
    std::vector<NativeX64FlowEdgeValueOperandEvidenceEntry> value_operand_evidence;
    // N113-G19: copy of G8 endpoint evidence for every live-through Value on
    // this exact edge (not limited to the G6 copy-candidate ValueId subset).
    std::uint32_t live_values_audited = 0U;
    std::uint32_t operand_anchors_validated = 0U;
    std::uint32_t endpoint_ownership_matches = 0U;
    std::uint32_t endpoint_ownership_mismatches = 0U;
    std::vector<NativeX64FlowEdgeEndpointValueEvidence> endpoint_value_evidence;
    bool exact_g9_schedule = false;
    bool endpoint_ownership_proven = false;
    bool scratch_liveness_proven = false;
    bool operation_operand_ownership_proven = false;
    bool proof_gate_passed = false;
    std::vector<std::uint32_t> value_ids;
    std::vector<std::string> blockers;
};

struct NativeX64FlowEdgeBundleEligibilityPlan {
    bool succeeded = false;
    std::size_t function_index = InvalidNativeFunctionIndex;
    std::uint32_t candidate_edges = 0U;
    std::uint32_t proof_gate_passed_edges = 0U;
    std::uint32_t blocked_edges = 0U;
    std::uint32_t schedule_blocked_edges = 0U;
    std::uint32_t endpoint_ownership_blocked_edges = 0U;
    std::uint32_t scratch_blocked_edges = 0U;
    std::uint32_t operation_operand_blocked_edges = 0U;
    NativeX64FlowOperandMismatchCounts mismatch_counts;
    std::vector<NativeX64FlowEdgeBundleEligibilityEntry> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeX64FlowEdgeBundleEligibilityPlan audit_native_x64_flow_edge_bundle_eligibility(
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const NativeX64FlowEdgePerEdgeScratchPlan& per_edge_scratch,
    const NativeX64FlowOperationOperandOwnershipPlan& operation_ownership,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    std::size_t function_index);

struct NativeBoundSpillReloadAction;

struct NativeX64EdgeTransferInsertionResult {
    bool succeeded = false;
    std::vector<std::uint8_t> code;
    std::vector<NativeX64RelativeBranchFixup> relative_branch_fixups;
    std::vector<NativeObjectRelocationInput> external_relocations;
    std::vector<X64FlowBranchAnchor> branch_anchors;
    std::vector<X64FlowEmitterAnchor> operation_anchors;
    std::vector<NativeX64CodeInsertionAdjustment> offset_adjustments;
    std::uint32_t edge_bundles_inserted = 0U;
    std::uint32_t machine_moves_inserted = 0U;
    std::uint32_t transfer_islands_emitted = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Inserts real parallel-copy bundles on exact CFG edges. Conditional-taken and
// unconditional edges are retargeted to appended islands. Conditional
// fallthrough and plain fallthrough edges receive an inline JMP redirect; all
// pre-existing rel32 branches that target the redirected boundary are rebased
// to the original instruction after the redirect so they do not enter the
// wrong edge's copy bundle. The caller must provide every internal rel32
// fixup in the function, not only N109A branch anchors. External COFF
// relocation records are also accepted and rebased, but not linker-patched.
NativeX64EdgeTransferInsertionResult insert_native_x64_edge_transfer_bundles(
    const std::vector<std::uint8_t>& code,
    const std::vector<NativeX64RelativeBranchFixup>& relative_branch_fixups,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<NativeX64EdgeTransferBundle>& bundles,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation gp_cycle_temporary,
    NativeRegisterLocation xmm_cycle_temporary,
    const NativeX64FrameLocationMap& frame_locations,
    const std::vector<NativeObjectRelocationInput>& external_relocations = {});

struct NativeX64SpillReloadInsertionResult {
    bool succeeded = false;
    std::vector<std::uint8_t> code;
    std::vector<NativeX64RelativeBranchFixup> relative_branch_fixups;
    std::vector<NativeObjectRelocationInput> external_relocations;
    std::vector<X64FlowEmitterAnchor> operation_anchors;
    std::vector<X64FlowBranchAnchor> branch_anchors;
    std::vector<NativeX64CodeInsertionAdjustment> offset_adjustments;
    std::uint32_t machine_actions_inserted = 0U;
    std::uint32_t machine_moves_inserted = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Materializes bound spill/reload actions at stable N108 operation boundaries.
// StoreBeforeCall uses the pre-argument-evaluation boundary; ReloadAfterCall
// uses the post-CALL/pre-cleanup boundary; definition stores and use reloads
// use the operation boundary after the local value has been produced/loaded.
// The allocator's selected registers must be reserved from unrelated
// expression temporaries by the caller; this routine performs code insertion
// and rebases rel32 fixups, operation anchors and CFG branch anchors. Targets
// at an insertion boundary map to the inserted action so control-flow entries
// execute required reloads before the original operation.
NativeX64SpillReloadInsertionResult insert_native_x64_spill_reload_actions(
    const std::vector<std::uint8_t>& code,
    const std::vector<NativeX64RelativeBranchFixup>& relative_branch_fixups,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::vector<NativeBoundSpillReloadAction>& bindings,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation gp_cycle_temporary,
    NativeRegisterLocation xmm_cycle_temporary,
    const NativeX64FrameLocationMap& frame_locations,
    const std::vector<NativeObjectRelocationInput>& external_relocations = {});

enum class NativeSpillReloadPlacement : std::uint8_t {
    BeforeOperation,
    AfterOperation
};

struct NativeBoundSpillReloadAction {
    NativeSpillReloadAction action;
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    NativeRegisterInstructionOriginKind origin_kind = NativeRegisterInstructionOriginKind::Synthetic;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeSpillReloadPlacement placement = NativeSpillReloadPlacement::BeforeOperation;
    // G1 operation-local cycle register. When assigned, the encoder must use
    // this register for this action instead of the function-wide fallback.
    NativeRegisterLocation cycle_temporary{};
    bool cycle_temporary_assigned = false;
    // The physical register bank may differ from the value class for GP
    // memory-to-memory MOVQ expansion through a volatile XMM1-XMM5 register.
    NativeRegisterClass cycle_temporary_register_class = NativeRegisterClass::GeneralPurpose;
};

// Registers held by AST expression temporaries at one exact emitted Flow
// operation. This replaces the unsafe function-wide union for G1 rebinding.
struct NativeX64ExpressionRegisterLiveness {
    std::vector<std::uint8_t> gp_registers;
    std::vector<std::uint8_t> xmm_registers;
};

struct NativeSpillReloadBindingResult {
    bool succeeded = false;
    std::vector<NativeBoundSpillReloadAction> bindings;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

struct NativeFlowSegmentSpillReloadResolutionResult {
    bool succeeded = false;
    std::vector<NativeBoundSpillReloadAction> bindings;
    NativeRegisterLocation gp_cycle_temporary;
    NativeRegisterLocation xmm_cycle_temporary;
    bool gp_cycle_temporary_required = false;
    bool xmm_cycle_temporary_required = false;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// N113-G1: rebind N112 action endpoints to the concrete locations that
// FunctionEmitter actually uses (N111), while keeping N112 Flow slots in
// disjoint frame-map namespaces. This is planner-only; it never inserts code.
struct NativeX64EmitterActionRebindingResult {
    bool succeeded = false;
    std::vector<NativeBoundSpillReloadAction> bindings;
    NativeX64FrameLocationMap frame_locations;
    NativeRegisterLocation gp_cycle_temporary;
    NativeRegisterLocation xmm_cycle_temporary;
    bool gp_cycle_temporary_required = false;
    bool xmm_cycle_temporary_required = false;
    std::uint32_t rebound_actions = 0U;
    std::uint32_t memory_to_memory_actions = 0U;
    std::uint32_t flow_spill_slots_rebased = 0U;
    std::uint32_t flow_home_slots_rebased = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// N113-C compares N112 action endpoints to the active N111 locations consumed
// by FunctionEmitter. Only register/register identity is directly provable:
// N112 and N111 stack-slot indices belong to separate allocation namespaces
// until an explicit frame map binds them.
enum class NativeX64ActionEndpointLocationStatus : std::uint8_t {
    RegisterMatch,
    RegisterMismatch,
    UnmappedSlotOrTemporary,
    NoPersistentBinding
};

struct NativeX64ActionLocationAuditEntry {
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    std::uint32_t value_id = 0U;
    NativeSpillReloadActionKind action_kind = NativeSpillReloadActionKind::StoreBeforeCall;
    NativeX64ActionEndpointLocationStatus source_status =
        NativeX64ActionEndpointLocationStatus::NoPersistentBinding;
    NativeX64ActionEndpointLocationStatus destination_status =
        NativeX64ActionEndpointLocationStatus::NoPersistentBinding;
};

struct NativeX64ActionLocationAuditResult {
    bool succeeded = false;
    std::uint32_t audited_actions = 0U;
    std::uint32_t actions_without_persistent_binding = 0U;
    std::uint32_t register_endpoint_matches = 0U;
    std::uint32_t register_endpoint_mismatches = 0U;
    std::uint32_t unmapped_slot_or_temporary_endpoints = 0U;
    std::uint32_t endpoints_without_persistent_binding = 0U;
    std::vector<NativeX64ActionLocationAuditEntry> entries;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Audits N112 actions without claiming they are safe to insert. The result is
// successful when every action has a structurally valid, unique identity;
// mismatched or unmapped endpoints are reported as data, not silently treated
// as equivalent locations.
NativeX64ActionLocationAuditResult audit_native_x64_flow_action_locations(
    const std::vector<NativeBoundSpillReloadAction>& bindings,
    const NativeX64MachineLocationPlan& machine_locations);

struct NativeX64FlowOperationLocationAuditEntry {
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    std::uint32_t value_id = 0U;
    bool before_operation = true;
    NativeX64ActionEndpointLocationStatus status =
        NativeX64ActionEndpointLocationStatus::NoPersistentBinding;
};

struct NativeX64FlowOperationLocationAuditResult {
    bool succeeded = false;
    std::uint32_t audited_operations = 0U;
    std::uint32_t audited_value_positions = 0U;
    std::uint32_t persistent_value_positions = 0U;
    std::uint32_t value_positions_without_persistent_binding = 0U;
    std::uint32_t register_position_matches = 0U;
    std::uint32_t register_position_mismatches = 0U;
    std::uint32_t unmapped_slot_or_temporary_positions = 0U;
    std::uint32_t unbound_value_positions = 0U;
    std::vector<NativeX64FlowOperationLocationAuditEntry> entries;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// N113-D audits N112 before/after operation value locations against the stable
// N111 active binding locations. It reports endpoint compatibility only; it
// does not rewrite emitter operands or insert any machine instructions.
NativeX64FlowOperationLocationAuditResult audit_native_x64_flow_operation_locations(
    const NativeRegisterFlowSegmentPlan& flow_plan,
    const NativeX64MachineLocationPlan& machine_locations);

// Binds abstract allocator actions to the exact stable Flow operation from
// N108/N109A. This validates placement and operand roles but does not insert
// machine instructions into the emitted function. N110B owns code insertion.
NativeSpillReloadBindingResult bind_native_spill_reload_actions_to_flow_operations(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterAllocationPlan& allocation);

// N113 planning seam: validates and converts the N112 per-Flow-operation
// actions to N110B insertion bindings, then chooses one caller-saved
// class-correct temporary from the allocator's R8-R11 / XMM1-XMM5 pools that is
// unused at every action boundary requiring a temporary. Expression-temporary
// registers must be supplied by the caller.
// This resolves metadata only; it does not mutate or emit FunctionEmitter code.
NativeFlowSegmentSpillReloadResolutionResult resolve_native_flow_segment_spill_reload_actions(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterFlowSegmentPlan& flow_plan,
    const std::vector<std::uint8_t>& reserved_expression_gp,
    const std::vector<std::uint8_t>& reserved_expression_xmm);

// Rebinds already-resolved N112 actions to exact N111 active locations and
// N108/N113-E emitter operands. Frame-map keys for Flow slots are rebased past
// the existing emitter spill/home namespaces and the lexical local area.
// The input frame maps must come from the same emitter/Flow allocations and
// local-area size. No machine-code bytes or emitter operands are modified.
NativeX64EmitterActionRebindingResult rebind_native_x64_flow_actions_to_emitter_locations(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterFlowSegmentPlan& flow_plan,
    const std::vector<NativeBoundSpillReloadAction>& resolved_bindings,
    const NativeX64MachineLocationPlan& machine_locations,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const NativeX64FrameLocationMap& emitter_frame_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    std::uint32_t emitter_spill_slot_count,
    std::uint32_t emitter_home_slot_count,
    std::uint32_t local_home_slot_count,
    const std::vector<std::uint8_t>& reserved_expression_gp,
    const std::vector<std::uint8_t>& reserved_expression_xmm,
    const std::map<std::uint64_t, NativeX64ExpressionRegisterLiveness>& expression_liveness = {});

} // namespace xfrontend
