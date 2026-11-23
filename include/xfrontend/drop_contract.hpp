#pragma once

#include "xfrontend/ownership_lowering.hpp"

#include <string>
#include <vector>

namespace xfrontend {

enum class DropRoutineKind {
    Trivial,
    Destructor,
    CustomDeleter
};

enum class DropFailureMode {
    NoThrow,
    MayFail
};

enum class DropFailurePolicy {
    RejectMayFail,
    TerminateOnFailure
};

enum class DropInvocationKind {
    NoOp,
    DestructorCall,
    CustomDeleterCall
};

// A frontend/backend contract for how a resource is finalized. This is metadata:
// resolving it does not call user code or emit machine instructions.
struct DropRoutineContract {
    std::string resource_type;
    DropRoutineKind kind = DropRoutineKind::Trivial;
    std::string routine_name;
    bool resolved = true;
    bool accessible = true;
    bool signature_valid = true;
    bool receiver_compatible = true;
    DropFailureMode failure_mode = DropFailureMode::NoThrow;
};

struct DropResolutionOptions {
    DropFailurePolicy may_fail_policy = DropFailurePolicy::RejectMayFail;
};

struct DropInvocation {
    DropInvocationKind kind = DropInvocationKind::NoOp;
    std::string owner_type;
    std::string resource_type;
    std::string routine_name;
    OwnershipActionGuard guard = OwnershipActionGuard::Never;
    DropFailureMode failure_mode = DropFailureMode::NoThrow;
    bool terminate_if_failed = false;
    FlowBlockId block_id = InvalidFlowBlockId;
    std::size_t action_index = 0;
    FlowVariableId variable_id = InvalidFlowVariableId;
    SourceSpan span;
};

struct DropInvocationBlock {
    FlowBlockId id = InvalidFlowBlockId;
    bool reachable = false;
    std::vector<DropInvocation> invocations;
};

struct DropInvocationFunction {
    std::string function_name;
    std::vector<FlowVariable> variables;
    std::vector<DropInvocationBlock> blocks;
};

struct DropContractResolutionResult {
    std::vector<DropInvocationFunction> functions;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Resolve every lowered DestroyResource action to an explicit resource cleanup
// contract. Type and routine identifiers follow X's ASCII case-insensitive
// identifier matching. Contracts are keyed by the payload type inside Unique[T] or
// UniqueArray[T], while each invocation also retains the concrete owner wrapper type.
// Unknown/ambiguous/invalid contracts fail closed.
// MayFail routines are rejected by default; the caller may explicitly request
// terminate-on-failure. Propagation and exception-unwinding semantics are not
// implemented in this stage.
DropContractResolutionResult resolve_ownership_drop_contracts(
    const OwnershipLoweringResult& lowered,
    const std::vector<DropRoutineContract>& contracts,
    DropResolutionOptions options = {});

} // namespace xfrontend
