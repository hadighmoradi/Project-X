#pragma once

#include "xfrontend/control_flow.hpp"
#include "xfrontend/native_register_analysis.hpp"
#include "xfrontend/win64_unwind_codegen.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xfrontend {

struct NativeRegisterFlowValueBinding {
    FlowVariableId flow_variable_id = InvalidFlowVariableId;
    std::uint32_t native_value_id = 0U;
    std::string name;
    std::string type_name;
    bool is_parameter = false;
    SourceSpan declaration_span;
};

struct NativeRegisterFlowFunction {
    std::string function_name;
    NativeRegisterFunctionIR register_ir;
    std::vector<NativeRegisterFlowValueBinding> bindings;
    // N109A control/edge identities are carried separately from the register
    // instruction IR so older allocator aggregate layouts remain unchanged.
    std::vector<FlowControlStatement> control_statements;
    std::vector<FlowEdge> edges;
};

struct NativeRegisterFlowLoweringResult {
    bool succeeded = false;
    std::vector<NativeRegisterFlowFunction> functions;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Lowers the real frontend CFG (including if/else, loops, break/continue,
// returns, and variable operations) into the machine-independent register IR.
// Unique-owner bookkeeping is deliberately excluded; it remains governed by
// the ownership pipeline. Scalar and pointer-like locals/parameters are kept.
NativeRegisterFlowLoweringResult lower_control_flow_to_native_register_ir(
    const CompilationUnit& unit);

struct NativeRegisterPhiIncoming {
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    std::uint32_t source_value_id = 0U;
};

struct NativeRegisterPhiNode {
    NativeRegisterBlockId block_id = InvalidNativeRegisterBlockId;
    std::uint32_t destination_value_id = 0U;
    std::vector<NativeRegisterPhiIncoming> incoming;
};

struct NativeRegisterPhiValidationResult {
    bool succeeded = false;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Validates block-parameter/Phi inputs: exactly one incoming per CFG
// predecessor, no foreign edges, no duplicate predecessor, and known values.
NativeRegisterPhiValidationResult validate_native_register_phi_nodes(
    const NativeRegisterFunctionIR& function,
    const std::vector<NativeRegisterPhiNode>& phis);

struct NativeRegisterParallelCopy {
    std::uint32_t value_id = 0U;
    NativeRegisterLocation source;
    NativeRegisterLocation destination;
};

struct NativeRegisterParallelCopyOperation {
    std::uint32_t value_id = 0U;
    NativeRegisterLocation source;
    NativeRegisterLocation destination;
    // True when this operation writes or reads the resolver's dedicated
    // cycle-storage location (a Temporary register or SpillSlot).
    bool uses_cycle_temporary = false;
};

struct NativeRegisterPhiEdgeTransfer {
    NativeRegisterBlockId predecessor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId successor_block_id = InvalidNativeRegisterBlockId;
    NativeRegisterBlockId insertion_block_id = InvalidNativeRegisterBlockId;
    bool critical_edge_split = false;
    std::vector<NativeRegisterParallelCopyOperation> operations;
};

struct NativeRegisterPhiEdgeLoweringResult {
    bool succeeded = false;
    NativeRegisterFunctionIR rewritten_function;
    std::vector<NativeRegisterPhiEdgeTransfer> edge_transfers;
    std::uint32_t critical_edges_split = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

struct NativeRegisterParallelCopyResult {
    bool succeeded = false;
    std::vector<NativeRegisterParallelCopyOperation> operations;
    std::uint32_t cycle_temporaries_used = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Resolves a simultaneous copy set into a deterministic sequential schedule.
// A caller-supplied Temporary register or dedicated SpillSlot is used only when
// a dependency cycle exists; it must not alias any source or destination in the input set.
// HomeSlot is deliberately excluded because canonical homes are not scratch storage.
// A SpillSlot here is an abstract cycle-storage choice only: its concrete frame
// mapping, non-aliasing with all active frame locations, and edge liveness remain
// separate caller/encoder obligations; this resolver does not reserve frame space.
NativeRegisterParallelCopyResult resolve_native_register_parallel_copies(
    const std::vector<NativeRegisterParallelCopy>& copies,
    NativeRegisterLocation cycle_temporary);

// Validates Phi inputs, groups all copies per CFG edge, schedules them through
// the parallel-copy resolver and splits critical edges in a rewritten CFG.
// Locations are supplied by the caller for each virtual value; this API does
// not pretend to perform instruction emission.
NativeRegisterPhiEdgeLoweringResult lower_native_register_phi_edges(
    const NativeRegisterFunctionIR& function,
    const std::vector<NativeRegisterPhiNode>& phis,
    const std::map<std::uint32_t, NativeRegisterLocation>& locations,
    NativeRegisterLocation cycle_temporary);

struct NativeRegisterCfgBranchEncodingResult {
    bool succeeded = false;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> block_offsets;
    std::uint32_t branch_edges_encoded = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Emits deterministic branch-only x64 stubs for an already validated CFG.
// For a two-successor block, RAX is the condition: nonzero selects successor[0],
// zero selects successor[1]. This is a control-transfer encoding contract, not
// a replacement for expression instruction selection.
NativeRegisterCfgBranchEncodingResult encode_native_cfg_branch_stubs(
    const NativeRegisterFunctionIR& function);


struct NativeRegisterVariableHome {
    FlowVariableId flow_variable_id = InvalidFlowVariableId;
    std::uint32_t value_id = 0U;
    std::uint32_t home_slot = 0U;
    std::string name;
    std::string type_name;
    bool is_parameter = false;
};

struct NativeRegisterVariableHomePlan {
    bool succeeded = false;
    std::uint32_t home_slot_count = 0U;
    std::uint32_t values_homed = 0U;
    std::vector<NativeRegisterVariableHome> homes;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Assigns every scalar/pointer source variable a deterministic canonical Win64
// home slot. N90 deliberately keeps one stable home per source binding: this
// is the correctness baseline for CFG joins and memory-SSA Phi coalescing.
NativeRegisterVariableHomePlan build_native_register_variable_home_plan(
    const NativeRegisterFlowFunction& function);

// Finalizes the home plan with the actual RBP-relative slot selected by the
// x64 emitter. Reuse is permitted when the backend's lexical lifetime allocator
// proves that the source bindings do not overlap.
NativeRegisterVariableHomePlan build_native_register_variable_home_plan(
    const NativeRegisterFlowFunction& function,
    const std::map<std::uint32_t, std::uint32_t>& physical_home_slots);

struct NativeRegisterHomePhiAlias {
    std::uint32_t synthetic_value_id = 0U;
    std::uint32_t source_home_value_id = 0U;
};

struct NativeRegisterHomePhiPlan {
    bool succeeded = false;
    std::vector<NativeRegisterPhiNode> phis;
    std::vector<NativeRegisterHomePhiAlias> home_aliases;
    std::uint32_t phi_count = 0U;
    std::uint32_t incoming_edges = 0U;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

// Creates memory-SSA Phi nodes for values live into multi-predecessor blocks.
// Each Phi is assigned the source variable's canonical home, so incoming values
// are coalesced without an edge MOV while stores on each predecessor preserve
// path-specific semantics.
NativeRegisterHomePhiPlan build_native_register_home_phi_plan(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterVariableHomePlan& homes);

struct NativeRegisterFlowAllocationResult {
    bool succeeded = false;
    NativeRegisterLivenessResult liveness;
    NativeRegisterIntervalAnalysis intervals;
    NativeRegisterSegmentAnalysis segments;
    NativeRegisterAllocationPlan allocation;
    std::vector<NativeRegisterIRDiagnostic> diagnostics;
};

NativeRegisterFlowAllocationResult analyze_and_allocate_native_flow_function(
    const NativeRegisterFlowFunction& function,
    NativeRegisterAllocationOptions options = {true, true, true, true});

struct Win64PrologueAction {
    enum class Kind {
        PushNonvolatileGpr,
        SetFramePointer,
        AllocateStack,
        SaveNonvolatileGpr,
        SaveNonvolatileXmm
    };

    Kind kind = Kind::AllocateStack;
    std::uint8_t code_offset = 0U;
    std::uint8_t register_index = 0U;
    // For save actions this is the non-negative offset from RSP after stack
    // allocation. It must obey the relevant UWOP scaling/alignment.
    std::uint32_t stack_offset_bytes = 0U;
    std::uint32_t allocation_bytes = 0U;
};

// Builds AMD64 UNWIND_INFO v1 for an actual prologue action sequence. Actions
// are supplied in execution order (ascending CodeOffset); records are emitted
// in the descending order required by PE/COFF unwind metadata. The result is a
// runtime-function record plus encoded .xdata payload; placing final RVAs into
// .pdata/.xdata remains the object/linker integration's responsibility.
Win64UnwindPlanResult build_win64_prologue_unwind_plan(
    std::size_t begin_offset,
    std::size_t end_offset,
    std::uint8_t prolog_size,
    std::uint8_t frame_register,
    std::uint8_t frame_offset,
    const std::vector<Win64PrologueAction>& actions);

} // namespace xfrontend
