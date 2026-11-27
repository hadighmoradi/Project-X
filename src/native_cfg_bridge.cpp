// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_cfg_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace xfrontend {
namespace {

void add_register_diagnostic(std::vector<NativeRegisterIRDiagnostic>& diagnostics,
                             const char* code, const char* message) {
    diagnostics.push_back(NativeRegisterIRDiagnostic{code, message});
}

std::string folded_type_name(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

NativeRegisterClass register_class_for_type(const std::string& type_name) {
    const std::string type = folded_type_name(type_name);
    if (type == "float" || type == "single" || type == "float32" ||
        type == "double" || type == "real" || type == "float64") {
        return NativeRegisterClass::FloatingPoint;
    }
    return NativeRegisterClass::GeneralPurpose;
}

using LocationKey = std::tuple<unsigned int, unsigned int, unsigned int>;

LocationKey location_key(const NativeRegisterLocation& location) {
    return LocationKey{static_cast<unsigned int>(location.kind),
                       static_cast<unsigned int>(location.register_index),
                       location.spill_slot};
}

bool same_location(const NativeRegisterLocation& left,
                   const NativeRegisterLocation& right) {
    return location_key(left) == location_key(right);
}

bool valid_location_kind(NativeRegisterLocationKind kind) {
    return kind == NativeRegisterLocationKind::Register ||
           kind == NativeRegisterLocationKind::SpillSlot ||
           kind == NativeRegisterLocationKind::HomeSlot ||
           kind == NativeRegisterLocationKind::Temporary;
}

bool is_nonvolatile_gpr(std::uint8_t reg) {
    return reg == 3U || reg == 5U || reg == 6U || reg == 7U ||
           (reg >= 12U && reg <= 15U);
}

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

} // namespace

NativeRegisterFlowLoweringResult lower_control_flow_to_native_register_ir(
    const CompilationUnit& unit) {
    NativeRegisterFlowLoweringResult result;
    const ControlFlowResult flow = analyze_control_flow(unit);
    if (!flow.succeeded()) {
        for (const auto& diagnostic : flow.diagnostics) {
            result.diagnostics.push_back(NativeRegisterIRDiagnostic{
                diagnostic.code, diagnostic.message});
        }
        return result;
    }

    for (const auto& graph : flow.functions) {
        NativeRegisterFlowFunction lowered;
        lowered.function_name = graph.function_name;
        lowered.control_statements = graph.control_statements;
        lowered.edges = graph.edges;
        lowered.expression_controls = graph.expression_controls;
        lowered.register_ir.entry_block = static_cast<NativeRegisterBlockId>(graph.entry_block);
        if (graph.blocks.empty() ||
            graph.blocks.size() > static_cast<std::size_t>(std::numeric_limits<NativeRegisterBlockId>::max())) {
            add_register_diagnostic(result.diagnostics, "N78E0001",
                "frontend CFG has no blocks or exceeds native block-ID capacity");
            return result;
        }

        std::set<FlowControlStatementId> control_statement_ids;
        for (const auto& control : graph.control_statements) {
            if (control.id == InvalidFlowControlStatementId ||
                !control_statement_ids.insert(control.id).second) {
                add_register_diagnostic(result.diagnostics, "N109AE0001",
                    "frontend CFG has an invalid or duplicate control-statement identity");
                return result;
            }
            if (control.parent_construct_id != InvalidFlowControlConstructId &&
                control_statement_ids.find(control.parent_construct_id) == control_statement_ids.end()) {
                add_register_diagnostic(result.diagnostics, "N109AE0002",
                    "frontend control statement references a parent construct that is not in pre-order");
                return result;
            }
        }
        std::set<FlowExpressionId> expression_ids;
        for (const auto& expression : graph.expression_controls) {
            if (expression.id == InvalidFlowExpressionId ||
                !expression_ids.insert(expression.id).second || expression.ast_expression == nullptr ||
                expression.kind != ExprKind::Binary ||
                (expression.operator_text != "&&" && expression.operator_text != "||") ||
                expression.ast_expression->kind != expression.kind ||
                expression.ast_expression->text != expression.operator_text ||
                expression.ast_expression->children.size() != 2U ||
                !expression.ast_expression->children[0] || !expression.ast_expression->children[1] ||
                expression.ast_expression->span.start.byte_offset != expression.span.start.byte_offset ||
                expression.ast_expression->span.end.byte_offset != expression.span.end.byte_offset) {
                add_register_diagnostic(result.diagnostics, "N109AE0008",
                    "frontend CFG has an invalid, duplicate, or unsupported logical-expression identity");
                return result;
            }
        }

        std::set<FlowEdgeId> edge_ids;
        std::set<std::pair<FlowBlockId, FlowBlockId>> edge_endpoints;
        std::set<std::pair<FlowBlockId, FlowBlockId>> feasible_edge_endpoints;
        for (const auto& edge : graph.edges) {
            if (edge.id == InvalidFlowEdgeId || !edge_ids.insert(edge.id).second ||
                edge.from_block_id >= graph.blocks.size() || edge.to_block_id >= graph.blocks.size() ||
                !edge_endpoints.insert({edge.from_block_id, edge.to_block_id}).second) {
                add_register_diagnostic(result.diagnostics, "N109AE0003",
                    "frontend CFG has an invalid, duplicate, or out-of-range edge identity");
                return result;
            }
            const auto& from = graph.blocks[edge.from_block_id];
            const auto& to = graph.blocks[edge.to_block_id];
            const bool in_successors = std::find(from.successors.begin(), from.successors.end(),
                                                 edge.to_block_id) != from.successors.end();
            const bool in_predecessors = std::find(to.predecessors.begin(), to.predecessors.end(),
                                                   edge.from_block_id) != to.predecessors.end();
            if (edge.analysis_feasible != in_successors ||
                edge.analysis_feasible != in_predecessors) {
                add_register_diagnostic(result.diagnostics, "N109AE0004",
                    "frontend CFG edge feasibility is inconsistent with predecessor/successor adjacency");
                return result;
            }
            if (edge.analysis_feasible) {
                feasible_edge_endpoints.insert({edge.from_block_id, edge.to_block_id});
            }
            const bool is_expression_edge_kind =
                edge.kind == FlowEdgeKind::ExpressionEvaluateRhs ||
                edge.kind == FlowEdgeKind::ExpressionShortCircuit ||
                edge.kind == FlowEdgeKind::ExpressionRhsMerge ||
                edge.kind == FlowEdgeKind::ExpressionShortMerge;
            if (edge.domain == FlowEdgeDomain::Statement) {
                if (edge.expression_id != InvalidFlowExpressionId || is_expression_edge_kind ||
                    !edge.analysis_feasible) {
                    add_register_diagnostic(result.diagnostics, "N109AE0008",
                        "statement edge has expression-only metadata or an unsupported infeasible statement state");
                    return result;
                }
            } else if (edge.domain == FlowEdgeDomain::Expression) {
                if (edge.expression_id == InvalidFlowExpressionId ||
                    expression_ids.find(edge.expression_id) == expression_ids.end() ||
                    edge.control_statement_id != InvalidFlowControlStatementId ||
                    edge.control_construct_id != InvalidFlowControlConstructId ||
                    !edge.machine_emission_required || !is_expression_edge_kind) {
                    add_register_diagnostic(result.diagnostics, "N109AE0008",
                        "expression edge is missing its logical-expression identity, kind, or emission obligation");
                    return result;
                }
            } else {
                add_register_diagnostic(result.diagnostics, "N109AE0008",
                    "frontend CFG edge has an unknown domain");
                return result;
            }
            if (edge.control_statement_id != InvalidFlowControlStatementId &&
                control_statement_ids.find(edge.control_statement_id) == control_statement_ids.end()) {
                add_register_diagnostic(result.diagnostics, "N109AE0005",
                    "frontend CFG edge references an unknown control statement");
                return result;
            }
            if (edge.control_construct_id != InvalidFlowControlConstructId &&
                control_statement_ids.find(edge.control_construct_id) == control_statement_ids.end()) {
                add_register_diagnostic(result.diagnostics, "N109AE0006",
                    "frontend CFG edge references an unknown control construct");
                return result;
            }
        }

        struct ExpressionEdgeGroup {
            const FlowEdge* rhs = nullptr;
            const FlowEdge* short_circuit = nullptr;
            const FlowEdge* rhs_merge = nullptr;
            const FlowEdge* short_merge = nullptr;
        };
        std::map<FlowExpressionId, ExpressionEdgeGroup> expression_edges;
        for (const auto& edge : graph.edges) {
            if (edge.domain != FlowEdgeDomain::Expression) continue;
            auto& group = expression_edges[edge.expression_id];
            const FlowEdge** slot = nullptr;
            switch (edge.kind) {
            case FlowEdgeKind::ExpressionEvaluateRhs: slot = &group.rhs; break;
            case FlowEdgeKind::ExpressionShortCircuit: slot = &group.short_circuit; break;
            case FlowEdgeKind::ExpressionRhsMerge: slot = &group.rhs_merge; break;
            case FlowEdgeKind::ExpressionShortMerge: slot = &group.short_merge; break;
            default: break;
            }
            if (slot == nullptr || *slot != nullptr) {
                add_register_diagnostic(result.diagnostics, "N109AE0009",
                    "expression control has an unknown or duplicate semantic edge kind");
                return result;
            }
            *slot = &edge;
        }
        if (expression_edges.size() != expression_ids.size()) {
            add_register_diagnostic(result.diagnostics, "N109AE0009",
                "logical-expression identity does not have exactly one complete edge group");
            return result;
        }
        for (const auto& item : expression_edges) {
            const auto& group = item.second;
            if (group.rhs == nullptr || group.short_circuit == nullptr ||
                group.rhs_merge == nullptr || group.short_merge == nullptr ||
                group.rhs->from_block_id != group.short_circuit->from_block_id ||
                group.rhs->to_block_id == group.short_circuit->to_block_id ||
                group.rhs_merge->to_block_id != group.short_merge->to_block_id ||
                !group.rhs_merge->analysis_feasible || !group.short_merge->analysis_feasible ||
                (!group.rhs->analysis_feasible && !group.short_circuit->analysis_feasible)) {
                add_register_diagnostic(result.diagnostics, "N109AE0009",
                    "logical-expression edge group does not form one feasible RHS/short-circuit split and merge");
                return result;
            }
        }

        std::set<std::pair<FlowBlockId, FlowBlockId>> successor_endpoints;
        std::set<std::pair<FlowBlockId, FlowBlockId>> predecessor_endpoints;
        std::size_t successor_count = 0U;
        for (const auto& block : graph.blocks) {
            successor_count += block.successors.size();
            for (const FlowBlockId successor : block.successors) {
                if (successor >= graph.blocks.size() ||
                    !successor_endpoints.insert({block.id, successor}).second) {
                    add_register_diagnostic(result.diagnostics, "N109AE0007",
                        "frontend CFG successor adjacency contains an invalid or duplicate pair");
                    return result;
                }
            }
            for (const FlowBlockId predecessor : block.predecessors) {
                if (predecessor >= graph.blocks.size() ||
                    !predecessor_endpoints.insert({predecessor, block.id}).second) {
                    add_register_diagnostic(result.diagnostics, "N109AE0007",
                        "frontend CFG predecessor adjacency contains an invalid or duplicate pair");
                    return result;
                }
            }
        }
        if (successor_count != feasible_edge_endpoints.size() ||
            successor_endpoints != feasible_edge_endpoints ||
            predecessor_endpoints != feasible_edge_endpoints) {
            add_register_diagnostic(result.diagnostics, "N109AE0007",
                "frontend CFG feasible edge set does not cover successor and predecessor adjacency exactly once");
            return result;
        }

        std::map<FlowVariableId, std::uint32_t> native_ids;
        for (const auto& variable : graph.variables) {
            // Unique owners and aggregate resource wrappers are represented by
            // the ownership/drop pipeline, not by scalar GP/XMM allocation.
            if (variable.is_unique_owner) continue;
            if (variable.id >= static_cast<FlowVariableId>(std::numeric_limits<std::uint32_t>::max() - 1U)) {
                add_register_diagnostic(result.diagnostics, "N78E0002",
                    "frontend variable ID exceeds native virtual-register capacity");
                return result;
            }
            const std::uint32_t native_id = static_cast<std::uint32_t>(variable.id + 1U);
            native_ids.emplace(variable.id, native_id);
            lowered.register_ir.values.push_back(NativeRegisterValueInfo{
                native_id, register_class_for_type(variable.type_name)});
            lowered.bindings.push_back(NativeRegisterFlowValueBinding{
                variable.id, native_id, variable.name, variable.type_name,
                variable.is_parameter, variable.declaration_span});
        }

        lowered.register_ir.blocks.resize(graph.blocks.size());
        for (std::size_t block_index = 0U; block_index < graph.blocks.size(); ++block_index) {
            const auto& source_block = graph.blocks[block_index];
            if (source_block.id != block_index) {
                add_register_diagnostic(result.diagnostics, "N78E0003",
                    "frontend CFG block IDs are not stable vector indices");
                return result;
            }
            auto& target_block = lowered.register_ir.blocks[block_index];
            target_block.id = static_cast<NativeRegisterBlockId>(block_index);
            for (const auto successor : source_block.successors) {
                if (successor >= graph.blocks.size()) {
                    add_register_diagnostic(result.diagnostics, "N78E0004",
                        "frontend CFG contains an out-of-range successor");
                    return result;
                }
                target_block.successors.push_back(static_cast<NativeRegisterBlockId>(successor));
            }

            NativeRegisterInstruction parameter_definitions;
            for (const auto& variable : graph.variables) {
                if (!variable.is_parameter || source_block.id != graph.entry_block) continue;
                const auto mapped = native_ids.find(variable.id);
                if (mapped != native_ids.end()) parameter_definitions.definitions.push_back(mapped->second);
            }
            if (!parameter_definitions.definitions.empty()) {
                target_block.instructions.push_back(std::move(parameter_definitions));
            }

            for (const auto& operation : source_block.operations) {
                NativeRegisterInstruction instruction;
                const auto mapped = native_ids.find(operation.variable_id);
                switch (operation.kind) {
                case FlowOperationKind::Read:
                    if (mapped != native_ids.end()) {
                        instruction.uses.push_back(mapped->second);
                        instruction.origin_kind = NativeRegisterInstructionOriginKind::FlowRead;
                    }
                    break;
                case FlowOperationKind::Declare:
                    if (mapped != native_ids.end()) {
                        if (operation.initialized) instruction.definitions.push_back(mapped->second);
                        instruction.origin_kind = NativeRegisterInstructionOriginKind::FlowDeclare;
                    }
                    break;
                case FlowOperationKind::Write:
                    if (mapped != native_ids.end()) {
                        instruction.definitions.push_back(mapped->second);
                        instruction.origin_kind = NativeRegisterInstructionOriginKind::FlowWrite;
                    }
                    break;
                case FlowOperationKind::Call:
                    instruction.is_call = true;
                    instruction.origin_kind = NativeRegisterInstructionOriginKind::FlowCall;
                    break;
                case FlowOperationKind::Return:
                    instruction.origin_kind = NativeRegisterInstructionOriginKind::FlowReturn;
                    break;
                case FlowOperationKind::OwnerCreate:
                case FlowOperationKind::OwnerTransfer:
                case FlowOperationKind::OwnerReset:
                case FlowOperationKind::OwnerCopy:
                case FlowOperationKind::OwnerDrop:
                    break;
                }
                if (instruction.origin_kind != NativeRegisterInstructionOriginKind::Synthetic) {
                    if (operation.stable_id == InvalidFlowOperationId ||
                        operation.operation_index == InvalidFlowOperationIndex) {
                        add_register_diagnostic(result.diagnostics, "N108E0004",
                            "frontend Flow operation is missing its deterministic source identity");
                        return result;
                    }
                    instruction.flow_operation_id = operation.stable_id;
                    instruction.flow_block_id = target_block.id;
                    instruction.flow_operation_index = operation.operation_index;
                    instruction.source_span = operation.span;
                    target_block.instructions.push_back(std::move(instruction));
                } else if (!instruction.definitions.empty() || !instruction.uses.empty() || instruction.is_call) {
                    target_block.instructions.push_back(std::move(instruction));
                }
            }
        }

        const auto validated = validate_native_register_function_ir(lowered.register_ir);
        if (!validated.succeeded) {
            for (const auto& diagnostic : validated.diagnostics) result.diagnostics.push_back(diagnostic);
            return result;
        }
        lowered.register_ir = validated.normalized_function;
        result.functions.push_back(std::move(lowered));
    }

    result.succeeded = true;
    return result;
}

NativeRegisterPhiValidationResult validate_native_register_phi_nodes(
    const NativeRegisterFunctionIR& function,
    const std::vector<NativeRegisterPhiNode>& phis) {
    NativeRegisterPhiValidationResult result;
    const auto validated = validate_native_register_function_ir(function);
    if (!validated.succeeded) {
        result.diagnostics = validated.diagnostics;
        return result;
    }
    const auto& normalized = validated.normalized_function;
    std::set<std::uint32_t> value_ids;
    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : normalized.values) {
        value_ids.insert(value.value_id);
        value_classes.emplace(value.value_id, value.register_class);
    }
    std::set<std::uint32_t> phi_destinations;
    std::set<std::uint32_t> instruction_definitions;
    for (const auto& block : normalized.blocks) {
        for (const auto& instruction : block.instructions) {
            instruction_definitions.insert(instruction.definitions.begin(), instruction.definitions.end());
        }
    }

    for (const auto& phi : phis) {
        if (phi.block_id >= normalized.blocks.size()) {
            add_register_diagnostic(result.diagnostics, "N79E0001", "Phi node targets an unknown basic block");
            return result;
        }
        if (value_ids.find(phi.destination_value_id) == value_ids.end()) {
            add_register_diagnostic(result.diagnostics, "N79E0002", "Phi destination is not a declared virtual value");
            return result;
        }
        if (!phi_destinations.insert(phi.destination_value_id).second ||
            instruction_definitions.find(phi.destination_value_id) != instruction_definitions.end()) {
            add_register_diagnostic(result.diagnostics, "N79E0003", "Phi destination is already defined by another Phi or instruction");
            return result;
        }
        const auto& block = normalized.blocks[phi.block_id];
        if (block.predecessors.empty() || phi.incoming.size() != block.predecessors.size()) {
            add_register_diagnostic(result.diagnostics, "N79E0004", "Phi must provide exactly one incoming value for every predecessor");
            return result;
        }
        std::set<NativeRegisterBlockId> incoming_blocks;
        for (const auto& incoming : phi.incoming) {
            if (value_ids.find(incoming.source_value_id) == value_ids.end()) {
                add_register_diagnostic(result.diagnostics, "N79E0005", "Phi incoming source is not a declared virtual value");
                return result;
            }
            if (value_classes.at(incoming.source_value_id) != value_classes.at(phi.destination_value_id)) {
                add_register_diagnostic(result.diagnostics, "N79E0008", "Phi source and destination register classes do not match");
                return result;
            }
            if (incoming.predecessor_block_id >= normalized.blocks.size() ||
                std::find(block.predecessors.begin(), block.predecessors.end(),
                          incoming.predecessor_block_id) == block.predecessors.end()) {
                add_register_diagnostic(result.diagnostics, "N79E0006", "Phi incoming source does not correspond to a CFG predecessor edge");
                return result;
            }
            if (!incoming_blocks.insert(incoming.predecessor_block_id).second) {
                add_register_diagnostic(result.diagnostics, "N79E0007", "Phi contains duplicate incoming predecessor edges");
                return result;
            }
        }
    }
    result.succeeded = true;
    return result;
}

NativeRegisterParallelCopyResult resolve_native_register_parallel_copies(
    const std::vector<NativeRegisterParallelCopy>& copies,
    NativeRegisterLocation cycle_temporary) {
    NativeRegisterParallelCopyResult result;
    if (cycle_temporary.kind != NativeRegisterLocationKind::Temporary &&
        cycle_temporary.kind != NativeRegisterLocationKind::SpillSlot) {
        add_register_diagnostic(result.diagnostics, "N80E0001",
            "parallel-copy cycle location must be a dedicated Temporary register or SpillSlot");
        return result;
    }

    std::vector<NativeRegisterParallelCopy> pending;
    std::set<LocationKey> destinations;
    std::set<LocationKey> all_locations;
    for (const auto& copy : copies) {
        if (!valid_location_kind(copy.source.kind) || !valid_location_kind(copy.destination.kind)) {
            add_register_diagnostic(result.diagnostics, "N80E0002", "parallel copy contains an invalid location kind");
            return result;
        }
        all_locations.insert(location_key(copy.source));
        all_locations.insert(location_key(copy.destination));
        if (!destinations.insert(location_key(copy.destination)).second) {
            add_register_diagnostic(result.diagnostics, "N80E0003", "parallel copy contains multiple writes to the same destination");
            return result;
        }
        if (!same_location(copy.source, copy.destination)) pending.push_back(copy);
    }
    if (all_locations.find(location_key(cycle_temporary)) != all_locations.end()) {
        add_register_diagnostic(result.diagnostics, "N80E0004", "parallel-copy cycle temporary aliases an input source or destination");
        return result;
    }

    while (!pending.empty()) {
        std::set<LocationKey> sources;
        for (const auto& copy : pending) sources.insert(location_key(copy.source));
        const auto safe = std::find_if(pending.begin(), pending.end(), [&sources](const auto& copy) {
            return sources.find(location_key(copy.destination)) == sources.end();
        });
        if (safe != pending.end()) {
            result.operations.push_back(NativeRegisterParallelCopyOperation{
                safe->value_id, safe->source, safe->destination,
                same_location(safe->source, cycle_temporary) ||
                same_location(safe->destination, cycle_temporary)});
            pending.erase(safe);
            continue;
        }

        // A cycle exists. Save the old contents of the first source and replace
        // all pending reads of that location by the cycle temporary.
        const NativeRegisterLocation saved_location = pending.front().source;
        const std::uint32_t saved_value = pending.front().value_id;
        result.operations.push_back(NativeRegisterParallelCopyOperation{
            saved_value, saved_location, cycle_temporary, true});
        ++result.cycle_temporaries_used;
        for (auto& copy : pending) {
            if (same_location(copy.source, saved_location)) copy.source = cycle_temporary;
        }
    }

    result.succeeded = true;
    return result;
}

NativeRegisterPhiEdgeLoweringResult lower_native_register_phi_edges(
    const NativeRegisterFunctionIR& function,
    const std::vector<NativeRegisterPhiNode>& phis,
    const std::map<std::uint32_t, NativeRegisterLocation>& locations,
    NativeRegisterLocation cycle_temporary) {
    NativeRegisterPhiEdgeLoweringResult result;
    const auto validation = validate_native_register_phi_nodes(function, phis);
    if (!validation.succeeded) {
        result.diagnostics = validation.diagnostics;
        return result;
    }
    if (cycle_temporary.kind != NativeRegisterLocationKind::Temporary &&
        cycle_temporary.kind != NativeRegisterLocationKind::SpillSlot) {
        add_register_diagnostic(result.diagnostics, "N85E0001",
            "Phi edge-copy cycle location must be a dedicated Temporary register or SpillSlot");
        return result;
    }

    using EdgeKey = std::pair<NativeRegisterBlockId, NativeRegisterBlockId>;
    std::map<EdgeKey, std::vector<NativeRegisterParallelCopy>> copies_by_edge;
    for (const auto& phi : phis) {
        const auto destination = locations.find(phi.destination_value_id);
        if (destination == locations.end()) {
            add_register_diagnostic(result.diagnostics, "N85E0002",
                "Phi destination has no allocated machine location");
            return result;
        }
        for (const auto& incoming : phi.incoming) {
            const auto source = locations.find(incoming.source_value_id);
            if (source == locations.end()) {
                add_register_diagnostic(result.diagnostics, "N85E0003",
                    "Phi incoming value has no allocated machine location");
                return result;
            }
            copies_by_edge[{incoming.predecessor_block_id, phi.block_id}].push_back(
                NativeRegisterParallelCopy{phi.destination_value_id,
                    source->second, destination->second});
        }
    }

    const auto function_validation = validate_native_register_function_ir(function);
    if (!function_validation.succeeded) {
        result.diagnostics = function_validation.diagnostics;
        return result;
    }
    result.rewritten_function = function_validation.normalized_function;
    for (const auto& edge_item : copies_by_edge) {
        const NativeRegisterBlockId predecessor = edge_item.first.first;
        const NativeRegisterBlockId successor = edge_item.first.second;
        if (predecessor >= result.rewritten_function.blocks.size() ||
            successor >= result.rewritten_function.blocks.size()) {
            add_register_diagnostic(result.diagnostics, "N85E0004",
                "Phi edge references a block outside the normalized CFG");
            return result;
        }
        const auto& predecessor_block = result.rewritten_function.blocks[predecessor];
        const auto& successor_block = result.rewritten_function.blocks[successor];
        const bool critical = predecessor_block.successors.size() > 1U &&
                              successor_block.predecessors.size() > 1U;
        NativeRegisterBlockId insertion = predecessor;
        if (critical) {
            if (result.rewritten_function.blocks.size() >=
                static_cast<std::size_t>(InvalidNativeRegisterBlockId)) {
                add_register_diagnostic(result.diagnostics, "N85E0005",
                    "critical-edge split exceeds native block-ID capacity");
                return result;
            }
            insertion = static_cast<NativeRegisterBlockId>(result.rewritten_function.blocks.size());
            NativeRegisterBasicBlock split;
            split.id = insertion;
            split.predecessors.push_back(predecessor);
            split.successors.push_back(successor);
            result.rewritten_function.blocks.push_back(std::move(split));

            auto& predecessor_successors = result.rewritten_function.blocks[predecessor].successors;
            const auto pred_edge = std::find(predecessor_successors.begin(), predecessor_successors.end(), successor);
            auto& successor_predecessors = result.rewritten_function.blocks[successor].predecessors;
            const auto succ_edge = std::find(successor_predecessors.begin(), successor_predecessors.end(), predecessor);
            if (pred_edge == predecessor_successors.end() || succ_edge == successor_predecessors.end()) {
                add_register_diagnostic(result.diagnostics, "N85E0006",
                    "critical edge disappeared while rewriting predecessor/successor lists");
                return result;
            }
            *pred_edge = insertion;
            *succ_edge = insertion;
            ++result.critical_edges_split;
        } else if (predecessor_block.successors.size() > 1U &&
                   successor_block.predecessors.size() == 1U) {
            insertion = successor;
        }

        const auto scheduled = resolve_native_register_parallel_copies(edge_item.second, cycle_temporary);
        if (!scheduled.succeeded) {
            result.diagnostics.insert(result.diagnostics.end(),
                scheduled.diagnostics.begin(), scheduled.diagnostics.end());
            return result;
        }
        result.edge_transfers.push_back(NativeRegisterPhiEdgeTransfer{
            predecessor, successor, insertion, critical, scheduled.operations});
    }

    const auto rewritten_validation = validate_native_register_function_ir(result.rewritten_function);
    if (!rewritten_validation.succeeded) {
        result.diagnostics = rewritten_validation.diagnostics;
        add_register_diagnostic(result.diagnostics, "N85E0007",
            "rewritten CFG failed validation after critical-edge splitting");
        return result;
    }
    result.rewritten_function = rewritten_validation.normalized_function;
    result.succeeded = true;
    return result;
}

NativeRegisterCfgBranchEncodingResult encode_native_cfg_branch_stubs(
    const NativeRegisterFunctionIR& function) {
    NativeRegisterCfgBranchEncodingResult result;
    const auto validation = validate_native_register_function_ir(function);
    if (!validation.succeeded) {
        result.diagnostics = validation.diagnostics;
        return result;
    }
    const auto& normalized = validation.normalized_function;
    result.block_offsets.resize(normalized.blocks.size(), 0U);
    struct Fixup { std::size_t displacement_offset; NativeRegisterBlockId target; };
    std::vector<Fixup> fixups;
    for (const auto& block : normalized.blocks) {
        if (result.bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            add_register_diagnostic(result.diagnostics, "N84E0001",
                "branch-stub layout exceeds 32-bit block-offset capacity");
            return result;
        }
        result.block_offsets[block.id] = static_cast<std::uint32_t>(result.bytes.size());
        if (block.successors.empty()) {
            result.bytes.push_back(0xC3U); // ret
        } else if (block.successors.size() == 1U) {
            result.bytes.push_back(0xE9U); // jmp rel32
            const std::size_t offset = result.bytes.size();
            result.bytes.insert(result.bytes.end(), 4U, 0U);
            fixups.push_back(Fixup{offset, block.successors[0]});
            ++result.branch_edges_encoded;
        } else if (block.successors.size() == 2U) {
            result.bytes.insert(result.bytes.end(), {0x48U, 0x85U, 0xC0U, 0x0FU, 0x84U}); // test rax,rax; jz
            const std::size_t false_offset = result.bytes.size();
            result.bytes.insert(result.bytes.end(), 4U, 0U);
            fixups.push_back(Fixup{false_offset, block.successors[1]});
            result.bytes.push_back(0xE9U);
            const std::size_t true_offset = result.bytes.size();
            result.bytes.insert(result.bytes.end(), 4U, 0U);
            fixups.push_back(Fixup{true_offset, block.successors[0]});
            result.branch_edges_encoded += 2U;
        } else {
            add_register_diagnostic(result.diagnostics, "N84E0002",
                "branch-stub encoder supports zero, one, or two successors per basic block");
            return result;
        }
    }

    for (const auto& fixup : fixups) {
        if (fixup.target >= result.block_offsets.size()) {
            add_register_diagnostic(result.diagnostics, "N84E0003",
                "branch-stub target block is outside the encoded CFG");
            return result;
        }
        const std::int64_t displacement = static_cast<std::int64_t>(result.block_offsets[fixup.target]) -
            static_cast<std::int64_t>(fixup.displacement_offset + 4U);
        if (displacement < std::numeric_limits<std::int32_t>::min() ||
            displacement > std::numeric_limits<std::int32_t>::max()) {
            add_register_diagnostic(result.diagnostics, "N84E0004",
                "branch-stub rel32 displacement is out of range");
            return result;
        }
        const std::uint32_t encoded = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
        for (unsigned byte = 0U; byte < 4U; ++byte)
            result.bytes[fixup.displacement_offset + byte] =
                static_cast<std::uint8_t>((encoded >> (byte * 8U)) & 0xffU);
    }
    result.succeeded = true;
    return result;
}

NativeRegisterVariableHomePlan build_native_register_variable_home_plan(
    const NativeRegisterFlowFunction& function) {
    NativeRegisterVariableHomePlan result;
    const auto validated = validate_native_register_function_ir(function.register_ir);
    if (!validated.succeeded) {
        result.diagnostics = validated.diagnostics;
        return result;
    }
    std::set<std::uint32_t> value_ids;
    for (const auto& value : validated.normalized_function.values) value_ids.insert(value.value_id);
    std::set<std::uint32_t> binding_ids;
    for (const auto& binding : function.bindings) {
        if (binding.native_value_id == 0U ||
            value_ids.find(binding.native_value_id) == value_ids.end() ||
            !binding_ids.insert(binding.native_value_id).second) {
            add_register_diagnostic(result.diagnostics, "N90E0001",
                "source binding has a missing, unknown, or duplicate native value ID");
            return result;
        }
        if (binding.declaration_span.start.byte_offset > binding.declaration_span.end.byte_offset) {
            add_register_diagnostic(result.diagnostics, "N90E0002",
                "source binding declaration span is reversed");
            return result;
        }
    }
    if (binding_ids.size() != value_ids.size()) {
        add_register_diagnostic(result.diagnostics, "N90E0003",
            "not every scalar native value has exactly one source-variable binding");
        return result;
    }

    std::vector<NativeRegisterVariableHome> ordered = function.bindings.empty()
        ? std::vector<NativeRegisterVariableHome>{} : std::vector<NativeRegisterVariableHome>{};
    ordered.reserve(function.bindings.size());
    std::vector<NativeRegisterFlowValueBinding> bindings = function.bindings;
    std::sort(bindings.begin(), bindings.end(), [](const auto& left, const auto& right) {
        return std::tie(left.declaration_span.source_id, left.declaration_span.start.byte_offset,
                        left.flow_variable_id, left.native_value_id) <
               std::tie(right.declaration_span.source_id, right.declaration_span.start.byte_offset,
                        right.flow_variable_id, right.native_value_id);
    });
    std::uint32_t slot = 0U;
    for (const auto& binding : bindings) {
        ordered.push_back(NativeRegisterVariableHome{
            binding.flow_variable_id, binding.native_value_id, slot++, binding.name,
            binding.type_name, binding.is_parameter});
    }
    result.homes = std::move(ordered);
    result.home_slot_count = slot;
    result.values_homed = static_cast<std::uint32_t>(result.homes.size());
    result.succeeded = true;
    return result;
}

NativeRegisterVariableHomePlan build_native_register_variable_home_plan(
    const NativeRegisterFlowFunction& function,
    const std::map<std::uint32_t, std::uint32_t>& physical_home_slots) {
    NativeRegisterVariableHomePlan result;
    const auto base = build_native_register_variable_home_plan(function);
    if (!base.succeeded) {
        result.diagnostics = base.diagnostics;
        return result;
    }
    std::uint32_t maximum_slot = 0U;
    bool has_slots = false;
    for (const auto& binding : function.bindings) {
        const auto found = physical_home_slots.find(binding.native_value_id);
        if (found == physical_home_slots.end()) {
            add_register_diagnostic(result.diagnostics, "N90E0010",
                "source binding has no physical stack-home assignment from x64 codegen");
            return result;
        }
        maximum_slot = std::max(maximum_slot, found->second);
        has_slots = true;
        result.homes.push_back(NativeRegisterVariableHome{
            binding.flow_variable_id, binding.native_value_id, found->second,
            binding.name, binding.type_name, binding.is_parameter});
    }
    if (physical_home_slots.size() != function.bindings.size()) {
        add_register_diagnostic(result.diagnostics, "N90E0011",
            "physical home map contains unknown or duplicate source-value assignments");
        return result;
    }
    result.values_homed = static_cast<std::uint32_t>(result.homes.size());
    result.home_slot_count = has_slots ? maximum_slot + 1U : 0U;
    result.succeeded = true;
    return result;
}

NativeRegisterHomePhiPlan build_native_register_home_phi_plan(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterVariableHomePlan& homes) {
    NativeRegisterHomePhiPlan result;
    const auto validated = validate_native_register_function_ir(function.register_ir);
    if (!validated.succeeded) {
        result.diagnostics = validated.diagnostics;
        return result;
    }
    if (!liveness.succeeded || !liveness.converged ||
        liveness.blocks.size() != validated.normalized_function.blocks.size() ||
        !homes.succeeded) {
        add_register_diagnostic(result.diagnostics, "N91E0001",
            "home Phi construction requires valid CFG liveness and a successful home plan");
        return result;
    }
    std::map<std::uint32_t, const NativeRegisterVariableHome*> home_by_value;
    for (const auto& home : homes.homes) home_by_value.emplace(home.value_id, &home);
    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    std::uint32_t next_value_id = 1U;
    for (const auto& value : validated.normalized_function.values) {
        value_classes.emplace(value.value_id, value.register_class);
        if (value.value_id >= next_value_id) {
            if (value.value_id == std::numeric_limits<std::uint32_t>::max()) {
                add_register_diagnostic(result.diagnostics, "N91E0002",
                    "no virtual value IDs remain for synthetic home Phi nodes");
                return result;
            }
            next_value_id = value.value_id + 1U;
        }
    }
    for (std::size_t block_index = 0U; block_index < validated.normalized_function.blocks.size(); ++block_index) {
        const auto& block = validated.normalized_function.blocks[block_index];
        if (block.predecessors.size() < 2U) continue;
        for (const std::uint32_t live_value : liveness.blocks[block_index].live_in) {
            const auto home = home_by_value.find(live_value);
            const auto type = value_classes.find(live_value);
            if (home == home_by_value.end() || type == value_classes.end()) {
                add_register_diagnostic(result.diagnostics, "N91E0003",
                    "live-in CFG value has no canonical source-variable home");
                return result;
            }
            if (next_value_id == 0U || next_value_id == std::numeric_limits<std::uint32_t>::max()) {
                add_register_diagnostic(result.diagnostics, "N91E0004",
                    "synthetic home Phi value ID overflow");
                return result;
            }
            std::vector<std::uint32_t> edge_versions;
            edge_versions.reserve(block.predecessors.size());
            for (std::size_t edge_index = 0U; edge_index < block.predecessors.size(); ++edge_index) {
                if (next_value_id == 0U || next_value_id == std::numeric_limits<std::uint32_t>::max()) {
                    add_register_diagnostic(result.diagnostics, "N91E0004",
                        "synthetic incoming Phi value ID overflow");
                    return result;
                }
                const std::uint32_t edge_version = next_value_id++;
                edge_versions.push_back(edge_version);
                result.home_aliases.push_back(NativeRegisterHomePhiAlias{edge_version, live_value});
            }
            if (next_value_id == 0U || next_value_id == std::numeric_limits<std::uint32_t>::max()) {
                add_register_diagnostic(result.diagnostics, "N91E0004",
                    "synthetic home Phi destination ID overflow");
                return result;
            }
            const std::uint32_t destination = next_value_id++;
            result.home_aliases.push_back(NativeRegisterHomePhiAlias{destination, live_value});
            result.phis.push_back(NativeRegisterPhiNode{static_cast<NativeRegisterBlockId>(block_index),
                destination, {}});
            auto& phi = result.phis.back();
            phi.incoming.reserve(block.predecessors.size());
            for (std::size_t edge_index = 0U; edge_index < block.predecessors.size(); ++edge_index) {
                phi.incoming.push_back(NativeRegisterPhiIncoming{block.predecessors[edge_index], edge_versions[edge_index]});
                ++result.incoming_edges;
            }
            result.phi_count++;
            (void)home; // Every edge version and Phi destination inherits the same canonical home.
        }
    }
    result.succeeded = true;
    return result;
}

NativeRegisterFlowAllocationResult analyze_and_allocate_native_flow_function(
    const NativeRegisterFlowFunction& function,
    NativeRegisterAllocationOptions options) {
    NativeRegisterFlowAllocationResult result;
    const auto validation = validate_native_register_function_ir(function.register_ir);
    if (!validation.succeeded) {
        result.diagnostics = validation.diagnostics;
        return result;
    }
    result.liveness = compute_native_register_liveness(validation.normalized_function);
    if (!result.liveness.succeeded || !result.liveness.converged) {
        result.diagnostics = result.liveness.diagnostics;
        add_register_diagnostic(result.diagnostics, "N81E0001", "flow-derived native register liveness did not converge");
        return result;
    }
    result.intervals = analyze_native_register_live_intervals(validation.normalized_function, result.liveness);
    if (!result.intervals.succeeded) {
        result.diagnostics = result.intervals.diagnostics;
        return result;
    }
    result.segments = split_native_register_live_ranges(
        validation.normalized_function, result.liveness, result.intervals);
    if (!result.segments.succeeded) {
        result.diagnostics = result.segments.diagnostics;
        return result;
    }
    result.allocation = build_native_register_allocation_plan(
        validation.normalized_function, result.liveness, result.intervals,
        result.segments, options);
    if (!result.allocation.succeeded) {
        result.diagnostics = result.allocation.diagnostics;
        return result;
    }
    result.succeeded = true;
    return result;
}

Win64UnwindPlanResult build_win64_prologue_unwind_plan(
    std::size_t begin_offset,
    std::size_t end_offset,
    std::uint8_t prolog_size,
    std::uint8_t frame_register,
    std::uint8_t frame_offset,
    const std::vector<Win64PrologueAction>& actions) {
    Win64UnwindPlanResult result;
    const auto add_error = [&result](const char* code, const char* message) {
        result.diagnostics.push_back(Diagnostic{code, message, {}});
    };
    if (begin_offset >= end_offset) {
        add_error("N83E0001", "runtime function range must be non-empty");
        return result;
    }
    if (prolog_size == 0U || frame_offset > 15U ||
        (frame_register != 0U && !is_nonvolatile_gpr(frame_register)) ||
        (frame_register == 0U && frame_offset != 0U)) {
        add_error("N83E0002", "prologue size or frame-register encoding is invalid");
        return result;
    }

    auto& function = result.runtime_function;
    function.begin_offset = begin_offset;
    function.end_offset = end_offset;
    function.prolog_size = prolog_size;
    function.frame_register = frame_register;
    function.frame_offset = frame_offset;
    std::set<std::uint8_t> offsets;
    bool saw_frame_setup = false;
    bool saw_stack_allocation = false;

    for (const auto& action : actions) {
        if (action.code_offset == 0U || action.code_offset > prolog_size ||
            !offsets.insert(action.code_offset).second) {
            add_error("N83E0003", "prologue action offsets must be unique and lie inside the prologue");
            return result;
        }
        Pe64DllUnwindCode code;
        code.code_offset = action.code_offset;
        switch (action.kind) {
        case Win64PrologueAction::Kind::PushNonvolatileGpr:
            if (!is_nonvolatile_gpr(action.register_index) || action.allocation_bytes != 0U ||
                action.stack_offset_bytes != 0U) {
                add_error("N83E0004", "push action requires a Win64 nonvolatile GPR and no extra operands");
                return result;
            }
            code.operation = Pe64DllUnwindOperation::PushNonVol;
            code.operation_info = action.register_index;
            break;
        case Win64PrologueAction::Kind::SetFramePointer:
            if (frame_register == 0U || saw_frame_setup || action.register_index != frame_register ||
                action.allocation_bytes != 0U || action.stack_offset_bytes != 0U) {
                add_error("N83E0005", "frame-pointer action does not match the declared frame register");
                return result;
            }
            saw_frame_setup = true;
            code.operation = Pe64DllUnwindOperation::SetFramePointer;
            break;
        case Win64PrologueAction::Kind::AllocateStack: {
            const std::uint32_t bytes = action.allocation_bytes;
            if (saw_stack_allocation || bytes < 8U || (bytes & 7U) != 0U ||
                action.register_index != 0U || action.stack_offset_bytes != 0U) {
                add_error("N83E0006", "stack allocation must be a single aligned nonzero prologue operation");
                return result;
            }
            saw_stack_allocation = true;
            if (bytes <= 128U) {
                code.operation = Pe64DllUnwindOperation::AllocSmall;
                code.operation_info = static_cast<std::uint8_t>((bytes - 8U) / 8U);
            } else if (bytes / 8U <= 0xffffU) {
                code.operation = Pe64DllUnwindOperation::AllocLarge;
                code.operation_info = 0U;
                code.operand = bytes;
            } else {
                code.operation = Pe64DllUnwindOperation::AllocLarge;
                code.operation_info = 1U;
                code.operand = bytes;
            }
            break;
        }
        case Win64PrologueAction::Kind::SaveNonvolatileGpr:
            if (!is_nonvolatile_gpr(action.register_index) || action.allocation_bytes != 0U ||
                (action.stack_offset_bytes & 7U) != 0U) {
                add_error("N83E0007", "saved GPR must be nonvolatile and use an eight-byte-aligned RSP offset");
                return result;
            }
            code.operation_info = action.register_index;
            code.operand = action.stack_offset_bytes;
            if (action.stack_offset_bytes <= 65535U * 8U) {
                code.operation = Pe64DllUnwindOperation::SaveNonVol;
            } else {
                code.operation = Pe64DllUnwindOperation::SaveNonVolFar;
            }
            break;
        case Win64PrologueAction::Kind::SaveNonvolatileXmm:
            if (action.register_index < 6U || action.register_index > 15U ||
                action.allocation_bytes != 0U || (action.stack_offset_bytes & 15U) != 0U) {
                add_error("N83E0008", "saved XMM register must be XMM6..XMM15 and use a 16-byte-aligned RSP offset");
                return result;
            }
            code.operation_info = action.register_index;
            code.operand = action.stack_offset_bytes;
            if (action.stack_offset_bytes <= 65535U * 16U) {
                code.operation = Pe64DllUnwindOperation::SaveXmm128;
            } else {
                code.operation = Pe64DllUnwindOperation::SaveXmm128Far;
            }
            break;
        }
        function.unwind_codes.push_back(code);
    }

    if (frame_register != 0U && !saw_frame_setup) {
        add_error("N83E0009", "declared frame register has no SET_FPREG prologue action");
        return result;
    }
    std::sort(function.unwind_codes.begin(), function.unwind_codes.end(),
        [](const auto& left, const auto& right) { return left.code_offset > right.code_offset; });

    std::vector<std::uint8_t> slots;
    std::size_t slot_count = 0U;
    for (const auto& code : function.unwind_codes) {
        slots.push_back(code.code_offset);
        slots.push_back(static_cast<std::uint8_t>((code.operation_info << 4U) |
            static_cast<std::uint8_t>(code.operation)));
        ++slot_count;
        switch (code.operation) {
        case Pe64DllUnwindOperation::AllocLarge:
            if (code.operation_info == 0U) {
                put16(slots, static_cast<std::uint16_t>(code.operand / 8U));
                ++slot_count;
            } else {
                put16(slots, static_cast<std::uint16_t>(code.operand & 0xffffU));
                put16(slots, static_cast<std::uint16_t>((code.operand >> 16U) & 0xffffU));
                slot_count += 2U;
            }
            break;
        case Pe64DllUnwindOperation::SaveNonVol:
            put16(slots, static_cast<std::uint16_t>(code.operand / 8U));
            ++slot_count;
            break;
        case Pe64DllUnwindOperation::SaveNonVolFar:
        case Pe64DllUnwindOperation::SaveXmm128Far:
            put16(slots, static_cast<std::uint16_t>(code.operand & 0xffffU));
            put16(slots, static_cast<std::uint16_t>((code.operand >> 16U) & 0xffffU));
            slot_count += 2U;
            break;
        case Pe64DllUnwindOperation::SaveXmm128:
            put16(slots, static_cast<std::uint16_t>(code.operand / 16U));
            ++slot_count;
            break;
        default:
            break;
        }
    }
    if (slot_count > 255U) {
        add_error("N83E0010", "UNWIND_INFO code-slot count exceeds 255");
        return result;
    }
    result.encoded_unwind_info = {
        1U, prolog_size, static_cast<std::uint8_t>(slot_count),
        static_cast<std::uint8_t>((frame_register & 0x0fU) | (frame_offset << 4U))};
    result.encoded_unwind_info.insert(result.encoded_unwind_info.end(), slots.begin(), slots.end());
    if ((slot_count & 1U) != 0U) {
        result.encoded_unwind_info.push_back(0U);
        result.encoded_unwind_info.push_back(0U);
    }
    return result;
}

} // namespace xfrontend
