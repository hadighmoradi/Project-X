// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/ownership_lowering.hpp"

#include <utility>

namespace xfrontend {
namespace {

void add_diagnostic(std::vector<Diagnostic>& diagnostics, const char* code,
                    const std::string& message, const SourceSpan& span) {
    diagnostics.push_back(Diagnostic{code, message, span});
}

bool valid_owner(const ControlFlowGraph& graph, FlowVariableId id) {
    return id < graph.variables.size() && graph.variables[id].is_unique_owner;
}

OwnershipActionGuard destruction_guard(FlowOwnershipState state) {
    switch (state) {
    case FlowOwnershipState::Live:
        return OwnershipActionGuard::Always;
    case FlowOwnershipState::MaybeLive:
    case FlowOwnershipState::MaybeDropped:
    case FlowOwnershipState::Unknown:
        return OwnershipActionGuard::IfLive;
    case FlowOwnershipState::Uninitialized:
    case FlowOwnershipState::Empty:
    case FlowOwnershipState::MaybeEmpty:
    case FlowOwnershipState::Dropped:
    case FlowOwnershipState::Borrowed:
        return OwnershipActionGuard::Never;
    }
    return OwnershipActionGuard::IfLive;
}

OwnershipLoweredAction make_action(OwnershipActionKind kind,
                                   const FlowOperation& operation,
                                   FlowBlockId block_id,
                                   std::size_t operation_index,
                                   OwnershipActionGuard guard = OwnershipActionGuard::Never) {
    OwnershipLoweredAction action;
    action.kind = kind;
    action.guard = guard;
    action.block_id = block_id;
    action.operation_index = operation_index;
    action.variable_id = operation.variable_id;
    action.related_variable_id = operation.related_variable_id;
    action.ownership_before = operation.ownership_before;
    action.ownership_after = operation.ownership_after;
    action.span = operation.span;
    return action;
}

OwnershipTransferTarget transfer_target(const BasicBlock& block, std::size_t index,
                                        const FlowOperation& operation) {
    if (operation.related_variable_id != InvalidFlowVariableId) {
        return operation.related_variable_id == operation.variable_id
            ? OwnershipTransferTarget::Self : OwnershipTransferTarget::Variable;
    }
    // Return cleanup is emitted between the transfer event and Return terminator.
    for (std::size_t next = index + 1U; next < block.operations.size(); ++next) {
        const auto kind = block.operations[next].kind;
        if (kind == FlowOperationKind::Return) return OwnershipTransferTarget::ReturnValue;
        if (kind == FlowOperationKind::OwnerTransfer &&
            block.operations[next].variable_id == operation.variable_id) break;
    }
    return OwnershipTransferTarget::ExpressionValue;
}

} // namespace

OwnershipLoweringResult lower_ownership_cleanup(const ControlFlowResult& control_flow) {
    OwnershipLoweringResult result;
    if (!control_flow.diagnostics.empty()) {
        result.diagnostics = control_flow.diagnostics;
        return result;
    }

    for (const auto& graph : control_flow.functions) {
        if (!graph.ownership_analysis_converged) {
            add_diagnostic(result.diagnostics, "XLOWER0001",
                           "Ownership cleanup lowering requires a converged ownership analysis",
                           graph.blocks.empty() ? SourceSpan{} : graph.blocks.front().span);
            continue;
        }

        OwnershipLoweredFunction lowered;
        lowered.function_name = graph.function_name;
        lowered.variables = graph.variables;
        lowered.blocks.reserve(graph.blocks.size());
        bool function_valid = true;

        for (const auto& block : graph.blocks) {
            OwnershipLoweredBlock lowered_block;
            lowered_block.id = block.id;
            lowered_block.reachable = block.reachable;
            if (!block.reachable) {
                lowered.blocks.push_back(std::move(lowered_block));
                continue;
            }

            for (std::size_t index = 0; index < block.operations.size(); ++index) {
                const FlowOperation& operation = block.operations[index];
                if (operation.kind != FlowOperationKind::OwnerCreate &&
                    operation.kind != FlowOperationKind::OwnerTransfer &&
                    operation.kind != FlowOperationKind::OwnerReset &&
                    operation.kind != FlowOperationKind::OwnerCopy &&
                    operation.kind != FlowOperationKind::OwnerDrop) {
                    continue;
                }
                if (!valid_owner(graph, operation.variable_id)) {
                    add_diagnostic(result.diagnostics, "XLOWER0003",
                                   "Ownership event references a non-owner or invalid variable",
                                   operation.span);
                    function_valid = false;
                    continue;
                }

                switch (operation.kind) {
                case FlowOperationKind::OwnerCreate:
                    lowered_block.actions.push_back(make_action(
                        OwnershipActionKind::EstablishResource, operation, block.id, index));
                    break;
                case FlowOperationKind::OwnerReset: {
                    const OwnershipActionGuard guard = destruction_guard(operation.ownership_before);
                    if (guard != OwnershipActionGuard::Never) {
                        lowered_block.actions.push_back(make_action(
                            OwnershipActionKind::DestroyResource, operation, block.id, index, guard));
                    }
                    lowered_block.actions.push_back(make_action(
                        OwnershipActionKind::ClearOwner, operation, block.id, index));
                    break;
                }
                case FlowOperationKind::OwnerTransfer: {
                    OwnershipLoweredAction action = make_action(
                        OwnershipActionKind::TransferResource, operation, block.id, index);
                    action.transfer_target = transfer_target(block, index, operation);
                    if (action.transfer_target == OwnershipTransferTarget::Variable &&
                        !valid_owner(graph, operation.related_variable_id)) {
                        add_diagnostic(result.diagnostics, "XLOWER0003",
                                       "Ownership transfer destination is not a Unique owner",
                                       operation.span);
                        function_valid = false;
                    }
                    lowered_block.actions.push_back(std::move(action));
                    break;
                }
                case FlowOperationKind::OwnerCopy:
                    add_diagnostic(result.diagnostics, "XLOWER0002",
                                   "Move-only Unique copy reached ownership lowering for '" +
                                       operation.name + "'",
                                   operation.span);
                    function_valid = false;
                    break;
                case FlowOperationKind::OwnerDrop: {
                    if (!operation.conditional) {
                        add_diagnostic(result.diagnostics, "XLOWER0004",
                                       "OwnerDrop must retain its conditional-live contract",
                                       operation.span);
                        function_valid = false;
                        break;
                    }
                    const OwnershipActionGuard guard = destruction_guard(operation.ownership_before);
                    if (guard != OwnershipActionGuard::Never) {
                        lowered_block.actions.push_back(make_action(
                            OwnershipActionKind::DestroyResource, operation, block.id, index, guard));
                    }
                    lowered_block.actions.push_back(make_action(
                        OwnershipActionKind::EndLifetime, operation, block.id, index));
                    break;
                }
                case FlowOperationKind::Read:
                case FlowOperationKind::Declare:
                case FlowOperationKind::Write:
                case FlowOperationKind::Return:
                case FlowOperationKind::Call:
                    break;
                }
            }
            lowered.blocks.push_back(std::move(lowered_block));
        }
        if (function_valid) result.functions.push_back(std::move(lowered));
    }
    return result;
}

} // namespace xfrontend
