#pragma once

#include "xfrontend/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

class SourceManager;

using FlowBlockId = std::size_t;
using FlowVariableId = std::size_t;
using FlowOperationId = std::uint64_t;
using FlowControlStatementId = std::uint64_t;
using FlowControlConstructId = std::uint64_t;
using FlowEdgeId = std::uint64_t;
constexpr FlowBlockId InvalidFlowBlockId = static_cast<FlowBlockId>(-1);
constexpr FlowVariableId InvalidFlowVariableId = static_cast<FlowVariableId>(-1);
constexpr FlowOperationId InvalidFlowOperationId = 0U;
constexpr FlowControlStatementId InvalidFlowControlStatementId = 0U;
constexpr FlowControlConstructId InvalidFlowControlConstructId = 0U;
constexpr FlowEdgeId InvalidFlowEdgeId = 0U;
constexpr std::uint32_t InvalidFlowOperationIndex = static_cast<std::uint32_t>(-1);


enum class FlowEdgeKind : std::uint8_t {
    Sequential,
    Return,
    IfTrue,
    IfFalse,
    IfThenMerge,
    IfElseMerge,
    LoopEntry,
    LoopBody,
    LoopExit,
    LoopBack,
    LoopBreak,
    LoopContinue,
    ForEachEntry,
    ForEachBody,
    ForEachExit,
    ForEachBack
};

struct FlowControlStatementIdentity {
    const Statement* statement = nullptr;
    FlowControlStatementId id = InvalidFlowControlStatementId;
    FlowControlConstructId parent_construct_id = InvalidFlowControlConstructId;
};

struct FlowControlStatement {
    FlowControlStatementId id = InvalidFlowControlStatementId;
    StatementKind kind = StatementKind::Block;
    FlowControlConstructId parent_construct_id = InvalidFlowControlConstructId;
    SourceSpan span{};
};

struct FlowEdge {
    FlowEdgeId id = InvalidFlowEdgeId;
    FlowBlockId from_block_id = InvalidFlowBlockId;
    FlowBlockId to_block_id = InvalidFlowBlockId;
    FlowEdgeKind kind = FlowEdgeKind::Sequential;
    FlowControlStatementId control_statement_id = InvalidFlowControlStatementId;
    FlowControlConstructId control_construct_id = InvalidFlowControlConstructId;
    SourceSpan span{};
};

// Deterministic pre-order IDs for control statements in one function body.
// IDs are function-local and independent of SourceSpan, which may be shared by
// synthetic CFG blocks or repeated source constructs.
std::vector<FlowControlStatementIdentity> enumerate_flow_control_statements(
    const Statement& root);

enum class FlowOperationKind {
    Read,
    Declare,
    Write,
    Return,
    // Ownership events are preserved in CFG order and consumed by the
    // flow-sensitive Unique state pass. They do not alter definite-init rules.
    OwnerCreate,
    OwnerTransfer,
    OwnerReset,
    OwnerCopy,
    // Conditional scope cleanup. The backend emits a destructor only when
    // the incoming ownership state contains a live resource.
    OwnerDrop,
    // Explicit call boundary used by machine-independent native register liveness.
    Call
};

// Abstract state of the resource held by a Unique wrapper at a CFG point.
// Take() empties the source wrapper; it does not move the wrapper binding.
enum class FlowOwnershipState {
    Uninitialized,
    Empty,
    Live,
    MaybeEmpty,
    MaybeLive,
    Dropped,
    MaybeDropped,
    Borrowed,
    Unknown
};

struct FlowVariable {
    FlowVariableId id = 0;
    std::string name;
    SourceSpan declaration_span;
    bool is_parameter = false;
    std::string type_name;
    std::string passing_mode;
    bool is_unique_owner = false;
};

struct FlowOperation {
    FlowOperationKind kind = FlowOperationKind::Read;
    FlowVariableId variable_id = 0;
    std::string name;
    SourceSpan span;
    bool initialized = false; // Meaningful for Declare.
    FlowVariableId related_variable_id = InvalidFlowVariableId;
    bool conditional = false; // Used by OwnerDrop; drop only when resource is live.
    FlowOwnershipState ownership_before = FlowOwnershipState::Unknown;
    FlowOwnershipState ownership_after = FlowOwnershipState::Unknown;
    // Deterministic identity within one function CFG: (block ID, operation ordinal).
    // Zero and UINT32_MAX are reserved as invalid sentinels.
    FlowOperationId stable_id = InvalidFlowOperationId;
    std::uint32_t operation_index = InvalidFlowOperationIndex;
};

struct BasicBlock {
    FlowBlockId id = InvalidFlowBlockId;
    std::vector<FlowOperation> operations;
    std::vector<FlowBlockId> successors;
    std::vector<FlowBlockId> predecessors;
    SourceSpan span; // Source anchor for the block, when one exists.
    bool reachable = false;
    // Ownership snapshots are aligned with ControlFlowGraph::variables.
    std::vector<FlowOwnershipState> ownership_in;
    std::vector<FlowOwnershipState> ownership_out;
};

struct ControlFlowGraph {
    std::string function_name;
    std::vector<FlowVariable> variables;
    std::vector<BasicBlock> blocks;
    FlowBlockId entry_block = InvalidFlowBlockId;
    FlowBlockId exit_block = InvalidFlowBlockId;
    bool has_reachable_return = false;
    bool has_reachable_fallthrough = false;
    // Synthetic blocks with no path from entry. Useful to IDEs and diagnostics
    // tooling; they are metadata, not errors by themselves.
    std::vector<FlowBlockId> unreachable_blocks;
    bool ownership_analysis_converged = true;
    std::size_t ownership_analysis_iterations = 0;
    std::vector<FlowControlStatement> control_statements;
    std::vector<FlowEdge> edges;
};

struct ControlFlowResult {
    std::vector<ControlFlowGraph> functions;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Builds explicit basic blocks and control-flow edges for each function body.
// Runs reachability, all-path return, forward definite-initialization and
// flow-sensitive Unique resource-state analysis. Ownership snapshots are
// exposed per basic block; scope exits emit conditional OwnerDrop events.
ControlFlowResult analyze_control_flow(const CompilationUnit& unit);
ControlFlowResult analyze_control_flow(const SourceManager& sources, SourceFileId source_id);

} // namespace xfrontend
