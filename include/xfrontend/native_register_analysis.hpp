// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/diagnostic.hpp"
#include "xfrontend/linear_scan_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

using NativeRegisterBlockId = std::uint32_t;
constexpr NativeRegisterBlockId InvalidNativeRegisterBlockId = static_cast<NativeRegisterBlockId>(-1);
constexpr std::uint64_t InvalidNativeFlowOperationId = 0U;
constexpr std::uint32_t InvalidNativeFlowOperationIndex = static_cast<std::uint32_t>(-1);

enum class NativeRegisterInstructionOriginKind : std::uint8_t {
    Synthetic,
    FlowRead,
    FlowDeclare,
    FlowWrite,
    FlowCall,
    FlowReturn
};

struct NativeRegisterValueInfo {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
};

// A compact, machine-independent register-analysis instruction. Definitions
// create or replace a virtual value; uses read it. Calls are explicit clobber
// boundaries for caller-saved allocation policies.
struct NativeRegisterInstruction {
    std::vector<std::uint32_t> definitions;
    std::vector<std::uint32_t> uses;
    bool is_call = false;
    // Synthetic instructions have no source origin. Flow-origin instructions
    // retain a stable CFG operation ID and exact source span for later emission.
    NativeRegisterInstructionOriginKind origin_kind = NativeRegisterInstructionOriginKind::Synthetic;
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    NativeRegisterBlockId flow_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t flow_operation_index = InvalidNativeFlowOperationIndex;
    SourceSpan source_span{};
};

struct NativeRegisterBasicBlock {
    NativeRegisterBlockId id = InvalidNativeRegisterBlockId;
    std::vector<NativeRegisterInstruction> instructions;
    std::vector<NativeRegisterBlockId> successors;
    std::vector<NativeRegisterBlockId> predecessors;
};

struct NativeRegisterFunctionIR {
    std::vector<NativeRegisterValueInfo> values;
    std::vector<NativeRegisterBasicBlock> blocks;
    NativeRegisterBlockId entry_block = InvalidNativeRegisterBlockId;
};

struct NativeRegisterIRDiagnostic {
    std::string code;
    std::string message;
};

struct NativeRegisterIRValidationResult {
    bool succeeded = false;
    NativeRegisterFunctionIR normalized_function;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeRegisterIRValidationResult validate_native_register_function_ir(
    const NativeRegisterFunctionIR& function);

struct NativeRegisterBlockLiveness {
    std::vector<std::uint32_t> live_in;
    std::vector<std::uint32_t> live_out;
};

struct NativeRegisterInstructionLiveness {
    std::vector<std::uint32_t> live_before;
    std::vector<std::uint32_t> live_after;
};

struct NativeRegisterLivenessResult {
    bool succeeded = false;
    bool converged = false;
    std::size_t iterations = 0U;
    std::vector<NativeRegisterBlockLiveness> blocks;
    std::vector<std::vector<NativeRegisterInstructionLiveness>> instructions;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

struct NativeRegisterInterferenceGraph {
    bool succeeded = false;
    std::vector<std::uint32_t> value_ids;
    std::vector<NativeRegisterInterferenceEdge> edges;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Builds same-class interference from per-instruction live-before/live-after
// sets and block live-in/live-out sets. Edges are canonicalized and sorted.
NativeRegisterInterferenceGraph build_native_register_interference_graph(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness);

// Backward may-liveness: IN = USE union (OUT - DEF), OUT = union(SUCC.IN).
// The solver reaches a fixed point for loops and irreducible control flow.
NativeRegisterLivenessResult compute_native_register_liveness(
    const NativeRegisterFunctionIR& function);

struct NativeRegisterIntervalAnalysis {
    bool succeeded = false;
    std::vector<std::uint32_t> block_start_positions;
    std::vector<std::vector<std::uint32_t>> instruction_positions;
    std::vector<NativeLiveInterval> intervals;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Builds conservative inclusive intervals from CFG liveness. Each instruction
// owns a before/after position pair; block gaps prevent accidental adjacency.
NativeRegisterIntervalAnalysis analyze_native_register_live_intervals(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness);

enum class NativeRegisterSplitReason {
    None,
    LiveRangeHole,
    CallBoundary
};

struct NativeRegisterLiveSegment {
    std::uint32_t segment_id = 0U;
    std::uint32_t value_id = 0U;
    NativeRegisterBlockId block_id = InvalidNativeRegisterBlockId;
    std::uint32_t start = 0U;
    std::uint32_t end = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterSplitReason split_before = NativeRegisterSplitReason::None;
    NativeRegisterSplitReason split_after = NativeRegisterSplitReason::None;
};

struct NativeRegisterSegmentAnalysis {
    bool succeeded = false;
    std::vector<NativeRegisterLiveSegment> segments;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Splits block-local live ranges at genuine holes and call clobber boundaries.
// Segments retain the originating virtual value ID and are independently
// assignable using segment_id as the allocator's unique interval key.
NativeRegisterSegmentAnalysis split_native_register_live_ranges(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals);

enum class NativeRegisterLocationKind {
    Register,
    SpillSlot,
    HomeSlot,
    Temporary
};

struct NativeRegisterLocation {
    NativeRegisterLocationKind kind = NativeRegisterLocationKind::Temporary;
    std::uint8_t register_index = 0U;
    std::uint32_t spill_slot = 0U;
};

enum class NativeSpillReloadActionKind {
    StoreBeforeCall,
    ReloadAfterCall,
    StoreAfterDefinition,
    ReloadBeforeUse
};

struct NativeSpillReloadAction {
    NativeSpillReloadActionKind kind = NativeSpillReloadActionKind::StoreBeforeCall;
    std::uint32_t value_id = 0U;
    NativeRegisterBlockId block_id = InvalidNativeRegisterBlockId;
    std::uint32_t instruction_index = 0U;
    std::uint32_t slot_index = 0U;
    NativeRegisterLocation source;
    NativeRegisterLocation destination;
};

struct NativeAllocatedSegment {
    NativeRegisterLiveSegment segment;
    NativeRegisterAssignment assignment;
};

struct NativeRegisterAllocationPlan {
    bool succeeded = false;
    std::vector<NativeAllocatedSegment> segments;
    std::vector<NativeSpillReloadAction> actions;
    std::uint32_t spill_slot_count = 0U;
    std::uint32_t home_slot_count = 0U;
    std::vector<std::uint8_t> used_callee_saved_gp;
    std::vector<std::uint8_t> used_callee_saved_xmm;
    NativeRegisterAllocationStats stats;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// N112 maps the allocator's block-local split segments back to stable Flow
// operation identities.  It deliberately remains a planning layer: it does
// not emit machine instructions or mutate FunctionEmitter state.
enum class NativeRegisterFlowActionPlacement : std::uint8_t {
    BeforeOperation,
    AfterOperation
};

struct NativeRegisterFlowSegmentLocation {
    NativeRegisterLiveSegment segment;
    NativeRegisterLocation location;
};

struct NativeRegisterFlowValueLocation {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterLocation location;
};

struct NativeRegisterFlowOperationLocation {
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    NativeRegisterBlockId flow_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t flow_operation_index = InvalidNativeFlowOperationIndex;
    std::uint32_t native_instruction_index = InvalidNativeFlowOperationIndex;
    std::uint32_t before_position = InvalidNativeFlowOperationIndex;
    std::uint32_t after_position = InvalidNativeFlowOperationIndex;
    std::vector<NativeRegisterFlowValueLocation> before_locations;
    std::vector<NativeRegisterFlowValueLocation> after_locations;
};

struct NativeRegisterFlowSegmentActionBinding {
    NativeSpillReloadAction action;
    std::uint64_t flow_operation_id = InvalidNativeFlowOperationId;
    NativeRegisterBlockId flow_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t flow_operation_index = InvalidNativeFlowOperationIndex;
    NativeRegisterInstructionOriginKind origin_kind = NativeRegisterInstructionOriginKind::Synthetic;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    NativeRegisterFlowActionPlacement placement = NativeRegisterFlowActionPlacement::BeforeOperation;
};

struct NativeRegisterFlowSegmentPlan {
    bool succeeded = false;
    std::vector<NativeRegisterFlowSegmentLocation> segments;
    std::vector<NativeRegisterFlowOperationLocation> operations;
    std::vector<NativeRegisterFlowSegmentActionBinding> actions;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Reconciles each before/after operation position with exactly one allocated
// segment and binds abstract spill/reload actions to N108 Flow operation IDs.
// Both native_instruction_index and flow_operation_index are retained because
// synthetic parameter definitions can shift the former without shifting the
// latter.
NativeRegisterFlowSegmentPlan build_native_register_flow_segment_plan(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals,
    const NativeRegisterSegmentAnalysis& segments,
    const NativeRegisterAllocationPlan& allocation);

// Allocates split segments, creates canonical homes for values live across
// caller-clobbering calls, and emits deterministic abstract spill/reload work
// items. Machine instruction emission consumes this plan in a later backend
// integration step; this API does not claim to emit stores or reloads itself.
NativeRegisterAllocationPlan build_native_register_allocation_plan(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals,
    const NativeRegisterSegmentAnalysis& segments,
    NativeRegisterAllocationOptions options = {true, true, true});

struct NativeWin64RegisterSave {
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    std::uint8_t register_index = 0U;
    std::int32_t frame_offset = 0; // Negative offset from RBP.
    std::uint32_t size_bytes = 0U;
};

struct NativeWin64FramePlan {
    bool succeeded = false;
    std::uint32_t local_area_bytes = 0U;
    std::uint32_t spill_area_bytes = 0U;
    std::uint32_t home_area_bytes = 0U;
    std::uint32_t callee_save_area_bytes = 0U;
    std::uint32_t frame_size_bytes = 0U;
    std::vector<std::int32_t> spill_slot_offsets;
    std::vector<std::int32_t> home_slot_offsets;
    std::vector<NativeWin64RegisterSave> prologue_saves;
    std::vector<NativeWin64RegisterSave> epilogue_restores;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Plans Win64 stack layout and nonvolatile GP/XMM preservation. Offsets are
// RBP-relative and negative. The caller still has to encode the save/restore
// instructions and matching unwind metadata.
NativeWin64FramePlan build_native_win64_frame_plan(
    const NativeRegisterAllocationPlan& allocation,
    std::uint32_t local_area_bytes);

} // namespace xfrontend
