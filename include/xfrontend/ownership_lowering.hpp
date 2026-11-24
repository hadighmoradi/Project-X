// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/control_flow.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace xfrontend {

enum class OwnershipActionKind {
    EstablishResource,
    DestroyResource,
    ClearOwner,
    TransferResource,
    EndLifetime
};

enum class OwnershipActionGuard {
    Never,
    Always,
    IfLive
};

enum class OwnershipTransferTarget {
    None,
    Variable,
    Self,
    ReturnValue,
    ExpressionValue
};

// Machine-independent ownership action produced from a flow-sensitive CFG.
// This is an IR contract for a future backend; it does not emit destructor code.
struct OwnershipLoweredAction {
    OwnershipActionKind kind = OwnershipActionKind::EstablishResource;
    OwnershipActionGuard guard = OwnershipActionGuard::Never;
    OwnershipTransferTarget transfer_target = OwnershipTransferTarget::None;
    FlowBlockId block_id = InvalidFlowBlockId;
    std::size_t operation_index = 0;
    FlowVariableId variable_id = InvalidFlowVariableId;
    FlowVariableId related_variable_id = InvalidFlowVariableId;
    FlowOwnershipState ownership_before = FlowOwnershipState::Unknown;
    FlowOwnershipState ownership_after = FlowOwnershipState::Unknown;
    SourceSpan span;
};

struct OwnershipLoweredBlock {
    FlowBlockId id = InvalidFlowBlockId;
    bool reachable = false;
    std::vector<OwnershipLoweredAction> actions;
};

struct OwnershipLoweredFunction {
    std::string function_name;
    std::vector<FlowVariable> variables;
    std::vector<OwnershipLoweredBlock> blocks;
};

struct OwnershipLoweringResult {
    std::vector<OwnershipLoweredFunction> functions;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Lowers CFG ownership events to deterministic, machine-independent actions.
// Reachable OwnerDrop events become conditional destruction + lifetime end;
// OwnerReset becomes optional destruction + explicit empty-state restoration.
// No native destructor invocation, exception cleanup, or LLVM IR is emitted.
OwnershipLoweringResult lower_ownership_cleanup(const ControlFlowResult& control_flow);

} // namespace xfrontend
