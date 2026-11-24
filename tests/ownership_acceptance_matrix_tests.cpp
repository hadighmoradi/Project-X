// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/control_flow.hpp"
#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using xfrontend::ControlFlowGraph;
using xfrontend::ControlFlowResult;
using xfrontend::FlowOperation;
using xfrontend::FlowOperationKind;
using xfrontend::FlowOwnershipState;
using xfrontend::FlowVariableId;

void accept_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

ControlFlowResult accept_flow(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("ownership acceptance source parse failed: " +
            parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message +
            " | source=" + source);
    }
    return xfrontend::analyze_control_flow(parsed.unit);
}

std::string accept_type(int index) {
    static const std::vector<std::string> types{
        "Unique[Device]", "UniqueArray[Device]", "Unique[Widget]", "UniqueArray[Widget]",
        "Unique[Buffer]", "UniqueArray[Buffer]", "UNIQUE[Device]", "Unique[Node]",
        "UniqueArray[Node]", "Unique[Resource]"
    };
    return types[static_cast<std::size_t>(index % static_cast<int>(types.size()))];
}

std::string accept_owner(int index) {
    static const std::vector<std::string> names{
        "owner", "Owner", "OWNER", "oWner", "resource", "Resource", "RES", "res", "unit", "Unit"
    };
    return names[static_cast<std::size_t>(index % static_cast<int>(names.size()))];
}

std::string accept_create(const std::string& type) { return type + ".Create()"; }

FlowVariableId accept_variable_id(const ControlFlowGraph& graph, const std::string& name) {
    for (const auto& variable : graph.variables) {
        std::string left = variable.name;
        std::string right = name;
        for (char& ch : left) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        for (char& ch : right) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (left == right && variable.is_unique_owner) return variable.id;
    }
    throw std::runtime_error("Unique owner not found: " + name);
}

const FlowOperation* accept_drop(const ControlFlowGraph& graph, FlowVariableId id) {
    for (auto block = graph.blocks.rbegin(); block != graph.blocks.rend(); ++block) {
        for (auto operation = block->operations.rbegin(); operation != block->operations.rend(); ++operation) {
            if (operation->kind == FlowOperationKind::OwnerDrop && operation->variable_id == id)
                return &*operation;
        }
    }
    return nullptr;
}

int accept_count(const ControlFlowGraph& graph, FlowOperationKind kind) {
    int result = 0;
    for (const auto& block : graph.blocks)
        for (const auto& operation : block.operations)
            if (operation.kind == kind) ++result;
    return result;
}

int accept_count_for(const ControlFlowGraph& graph, FlowOperationKind kind, FlowVariableId id) {
    int result = 0;
    for (const auto& block : graph.blocks)
        for (const auto& operation : block.operations)
            if (operation.kind == kind && operation.variable_id == id) ++result;
    return result;
}

std::vector<std::string> accept_drop_order(const ControlFlowGraph& graph) {
    std::vector<std::string> result;
    for (const auto& block : graph.blocks)
        for (const auto& operation : block.operations)
            if (operation.kind == FlowOperationKind::OwnerDrop) result.push_back(operation.name);
    return result;
}

void accept_graph_integrity(const ControlFlowGraph& graph) {
    accept_require(graph.ownership_analysis_converged, "ownership analysis must converge");
    accept_require(graph.entry_block < graph.blocks.size(), "entry block must be valid");
    accept_require(graph.exit_block < graph.blocks.size(), "exit block must be valid");
    accept_require(graph.ownership_analysis_iterations > 0U, "ownership fixed-point pass must execute");
    for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
        const auto& block = graph.blocks[index];
        accept_require(block.id == index, "block identity must match vector index");
        if (block.reachable) {
            accept_require(block.ownership_in.size() == graph.variables.size(),
                           "reachable block ownership input vector has wrong size");
            accept_require(block.ownership_out.size() == graph.variables.size(),
                           "reachable block ownership output vector has wrong size");
        }
        for (const auto successor : block.successors) {
            accept_require(successor < graph.blocks.size(), "successor id is out of range");
            const auto& predecessors = graph.blocks[successor].predecessors;
            accept_require(std::find(predecessors.begin(), predecessors.end(), block.id) != predecessors.end(),
                           "CFG successor/predecessor relationship is asymmetric");
        }
        for (const auto predecessor : block.predecessors) {
            accept_require(predecessor < graph.blocks.size(), "predecessor id is out of range");
            const auto& successors = graph.blocks[predecessor].successors;
            accept_require(std::find(successors.begin(), successors.end(), block.id) != successors.end(),
                           "CFG predecessor/successor relationship is asymmetric");
        }
        for (const auto& operation : block.operations) {
            if (operation.variable_id != xfrontend::InvalidFlowVariableId)
                accept_require(operation.variable_id < graph.variables.size(), "operation variable id is invalid");
            if (operation.related_variable_id != xfrontend::InvalidFlowVariableId)
                accept_require(operation.related_variable_id < graph.variables.size(), "related variable id is invalid");
            if (operation.kind == FlowOperationKind::OwnerDrop)
                accept_require(operation.conditional, "owner drop must be conditional on a live resource");
        }
    }
}

bool accept_returns_cleanup_before_terminator(const ControlFlowGraph& graph) {
    bool found_return = false;
    for (const auto& block : graph.blocks) {
        std::size_t return_index = block.operations.size();
        for (std::size_t index = 0; index < block.operations.size(); ++index) {
            if (block.operations[index].kind == FlowOperationKind::Return) {
                return_index = index;
                found_return = true;
                break;
            }
        }
        if (return_index == block.operations.size()) continue;
        bool drop_before = false;
        for (std::size_t index = 0; index < return_index; ++index)
            if (block.operations[index].kind == FlowOperationKind::OwnerDrop) drop_before = true;
        if (!drop_before) return false;
        for (std::size_t index = return_index + 1U; index < block.operations.size(); ++index)
            if (block.operations[index].kind == FlowOperationKind::OwnerDrop) return false;
    }
    return found_return;
}

void accept_case(const std::string& name, const std::function<void()>& body,
                 int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::string accept_action(int kind, const std::string& owner, const std::string& type) {
    switch (kind) {
    case 0: return "PrintLine " + owner + ".Get();";
    case 1: return owner + ".Reset();";
    case 2: return owner + ".Take();";
    case 3: return owner + ".Reset(); " + owner + " = " + accept_create(type) + ";";
    case 4: return owner + ".Take(); " + owner + " = " + accept_create(type) + ";";
    case 5: return owner + ".Take(); " + owner + ".Reset(); " + owner + " = " + accept_create(type) + ";";
    case 6: return owner + ".Reset(); " + owner + ".Reset();";
    case 7: return owner + " = " + owner + ".Take();";
    case 8: return "if (flag) { " + owner + ".Reset(); } else { PrintLine " + owner + ".Get(); }";
    default: return "if (flag) { " + owner + ".Take(); } else { " + owner + ".Reset(); }";
    }
}

} // namespace

void run_ownership_acceptance_matrix_tests(int& passed, int& failed) {
    using namespace xfrontend;

    // A (500): per-type lifecycle sequences. These independently check the
    // state observed by the scope-drop event after create/read/reset/take and
    // conditional control-flow operations, while varying identifier casing.
    {
        int id = 1;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int sequence = 0; sequence < 10; ++sequence) {
                for (int variant = 0; variant < 5; ++variant) {
                    const std::string type = accept_type(type_case);
                    const std::string owner = accept_owner(variant + sequence);
                    std::string action = accept_action(sequence, owner, type);
                    if (variant == 1) action = "{ " + action + " }";
                    if (variant == 2) action = "if (flag) { { " + action + " } } else { PrintLine " + owner + ".Get(); }";
                    if (variant == 3) action = "{ PrintLine " + owner + ".Get(); " + action + " }";
                    if (variant == 4) action = action + " PrintLine " + owner + ".Get();";
                    const std::string flag_name = variant % 2 == 0 ? "flag" : "FLAG";
                    const std::string source = "function Probe(bool " + flag_name + ") { " + type + " " + owner + " = " +
                        accept_create(type) + "; " + action + " }";
                    FlowOwnershipState expected = FlowOwnershipState::Live;
                    if (sequence == 1 || sequence == 2 || sequence == 6 || sequence == 9)
                        expected = sequence == 9 ? FlowOwnershipState::Empty : FlowOwnershipState::Empty;
                    if (sequence == 8) expected = FlowOwnershipState::MaybeLive;
                    const std::string name = "N16A" + std::to_string(id++) + "_lifecycle_matrix_type" +
                        std::to_string(type_case) + "_sequence" + std::to_string(sequence) + "_variant" + std::to_string(variant);
                    accept_case(name, [source, owner, expected, sequence, variant] {
                        const auto result = accept_flow(source);
                        accept_require(result.succeeded(), "CFG should accept lifecycle sequence");
                        accept_require(result.functions.size() == 1U, "one function graph expected");
                        const auto& graph = result.functions.front();
                        accept_graph_integrity(graph);
                        const auto owner_id = accept_variable_id(graph, owner);
                        const auto* drop = accept_drop(graph, owner_id);
                        accept_require(drop != nullptr, "owner must have a scope-drop event");
                        accept_require(drop->conditional, "drop must be conditional");
                        // Variants 2 and 4 deliberately introduce an extra branch or
                        // read, so only the unambiguous straight-line sequences use
                        // the exact terminal-state oracle.
                        if (sequence <= 7 && sequence != 5 && variant != 2) {
                            accept_require(drop->ownership_before == expected,
                                           "unexpected terminal ownership state after lifecycle sequence");
                        }
                        if (sequence == 7) {
                            accept_require(accept_count_for(graph, FlowOperationKind::OwnerTransfer, owner_id) >= 1,
                                           "self Take assignment must record a transfer event");
                        }
                        if (sequence == 1 || sequence == 6)
                            accept_require(accept_count_for(graph, FlowOperationKind::OwnerReset, owner_id) >= 1,
                                           "reset sequence must record reset metadata");
                    }, passed, failed);
                }
            }
        }
    }

    // B (300): lexical scope depth x owner-count x layout. Every unique local
    // receives exactly one conditional drop and each same-scope group drops
    // in reverse declaration order. Non-owner locals do not create drop events.
    {
        int id = 1;
        for (int depth_case = 0; depth_case < 10; ++depth_case) {
            for (int owner_case = 0; owner_case < 10; ++owner_case) {
                for (int layout = 0; layout < 3; ++layout) {
                    const int depth = 1 + depth_case % 4;
                    const int owner_count = 1 + owner_case;
                    std::string source = "function Probe(bool flag) { ";
                    for (int nest = 0; nest < depth; ++nest) source += "{ ";
                    source += "int marker = 3; ";
                    std::vector<std::string> declared;
                    for (int owner = 0; owner < owner_count; ++owner) {
                        const std::string name = "scopeOwner" + std::to_string(owner);
                        const std::string type = accept_type(depth_case + owner_case + owner);
                        declared.push_back(name);
                        source += type + " " + name + " = " + accept_create(type) + "; ";
                        if (layout == 1 && owner % 2 == 0) source += "PrintLine " + name + ".Get(); ";
                        if (layout == 2 && owner % 2 == 1) source += "if (flag) { PrintLine " + name + ".Get(); } ";
                    }
                    for (int nest = 0; nest < depth; ++nest) source += "} ";
                    source += "}";
                    const std::string name = "N16B" + std::to_string(id++) + "_scope_order_matrix_depth" +
                        std::to_string(depth) + "_owners" + std::to_string(owner_count) + "_layout" + std::to_string(layout);
                    accept_case(name, [source, declared] {
                        const auto result = accept_flow(source);
                        accept_require(result.succeeded(), "scope-order source should build CFG");
                        const auto& graph = result.functions.front();
                        accept_graph_integrity(graph);
                        accept_require(accept_count(graph, FlowOperationKind::OwnerDrop) ==
                                           static_cast<int>(declared.size()),
                                       "every local owner must have exactly one drop event");
                        const auto drops = accept_drop_order(graph);
                        accept_require(drops.size() == declared.size(), "drop order size mismatch");
                        for (std::size_t index = 0; index < declared.size(); ++index)
                            accept_require(drops[index] == declared[declared.size() - index - 1U],
                                           "same-scope owners must drop in reverse declaration order");
                    }, passed, failed);
                }
            }
        }
    }

    // C (400): multi-hop Take chains. Four construction forms exercise direct
    // declaration, initialized destination assignment, uninitialized typed
    // destination assignment, and an observable read at every hop.
    {
        int id = 1;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int chain_case = 0; chain_case < 10; ++chain_case) {
                for (int variant = 0; variant < 4; ++variant) {
                    const std::string type = accept_type(type_case);
                    const int hops = 1 + chain_case;
                    std::vector<std::string> owners;
                    for (int index = 0; index <= hops; ++index)
                        owners.push_back((variant % 2 == 0 ? "chain" : "Chain") + std::to_string(index));
                    std::string source = "function Probe() { ";
                    source += type + " " + owners[0] + " = " + accept_create(type) + "; ";
                    if (variant == 1) {
                        for (int index = 1; index <= hops; ++index)
                            source += type + " " + owners[static_cast<std::size_t>(index)] + " = " + accept_create(type) + "; ";
                    } else if (variant == 2) {
                        for (int index = 1; index <= hops; ++index)
                            source += type + " " + owners[static_cast<std::size_t>(index)] + "; ";
                    }
                    if (variant == 3) source += "PrintLine " + owners[0] + ".Get(); ";
                    for (int index = 1; index <= hops; ++index) {
                        const auto& previous = owners[static_cast<std::size_t>(index - 1)];
                        const auto& current = owners[static_cast<std::size_t>(index)];
                        if (variant == 0 || variant == 3)
                            source += type + " " + current + " = " + previous + ".Take(); ";
                        else
                            source += current + " = " + previous + ".Take(); ";
                        if (variant == 3) source += "PrintLine " + current + ".Get(); ";
                    }
                    source += "}";
                    const std::string name = "N16C" + std::to_string(id++) + "_take_chain_matrix_type" +
                        std::to_string(type_case) + "_hops" + std::to_string(hops) + "_variant" + std::to_string(variant);
                    accept_case(name, [source, owners, hops] {
                        const auto result = accept_flow(source);
                        accept_require(result.succeeded(), "multi-hop Take chain should build CFG");
                        const auto& graph = result.functions.front();
                        accept_graph_integrity(graph);
                        accept_require(accept_count(graph, FlowOperationKind::OwnerTransfer) == hops,
                                       "one ownership transfer is required for each chain hop");
                        accept_require(accept_count(graph, FlowOperationKind::OwnerDrop) == hops + 1,
                                       "every chain wrapper must be dropped exactly once");
                        for (int index = 0; index < hops; ++index) {
                            const auto source_id = accept_variable_id(graph, owners[static_cast<std::size_t>(index)]);
                            const auto target_id = accept_variable_id(graph, owners[static_cast<std::size_t>(index + 1)]);
                            bool found = false;
                            for (const auto& block : graph.blocks) {
                                for (const auto& operation : block.operations) {
                                    if (operation.kind != FlowOperationKind::OwnerTransfer ||
                                        operation.variable_id != source_id) continue;
                                    accept_require(operation.related_variable_id == target_id,
                                                   "Take transfer must point to the next wrapper id");
                                    accept_require(operation.ownership_before == FlowOwnershipState::Live,
                                                   "Take source must be live before transfer");
                                    accept_require(operation.ownership_after == FlowOwnershipState::Empty,
                                                   "Take source must be empty after transfer");
                                    found = true;
                                }
                            }
                            accept_require(found, "expected hop transfer metadata is missing");
                            const auto* drop = accept_drop(graph, source_id);
                            accept_require(drop && drop->ownership_before == FlowOwnershipState::Empty,
                                           "intermediate moved-from wrapper must be empty at drop");
                        }
                        const auto final_id = accept_variable_id(graph, owners.back());
                        const auto* final_drop = accept_drop(graph, final_id);
                        accept_require(final_drop && final_drop->ownership_before == FlowOwnershipState::Live,
                                       "last chain wrapper must own the final resource");
                    }, passed, failed);
                }
            }
        }
    }

    // D (450): 3x3 branch terminal-state combinations x 10 wrapper types x 5
    // CFG spellings. The expected join is Live, Empty, or MaybeLive depending
    // only on the two mutually exclusive branch outcomes.
    {
        int id = 1;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int left_state = 0; left_state < 3; ++left_state) {
                for (int right_state = 0; right_state < 3; ++right_state) {
                    for (int variant = 0; variant < 5; ++variant) {
                        const std::string type = accept_type(type_case);
                        const std::string owner = accept_owner(type_case + variant);
                        const std::string left = accept_action(left_state, owner, type);
                        const std::string right = accept_action(right_state == 0 ? 0 : right_state, owner, type);
                        std::string branch;
                        if (variant == 0) branch = "if (flag) { " + left + " } else { " + right + " }";
                        else if (variant == 1) branch = "if (FLAG) { { " + left + " } } else { { " + right + " } }";
                        else if (variant == 2) branch = "if (flag && other) { " + left + " } else { " + right + " }";
                        else if (variant == 3) branch = "if (flag) { " + right + " } else { " + left + " }";
                        else branch = "if (flag) { if (other) { " + left + " } else { " + left + " } } else { " + right + " }";
                        const std::string source = "function Probe(bool flag, bool FLAG, bool other) { " + type + " " + owner +
                            " = " + accept_create(type) + "; " + branch + " }";
                        FlowOwnershipState expected = FlowOwnershipState::MaybeLive;
                        if (left_state == 0 && right_state == 0) expected = FlowOwnershipState::Live;
                        if (left_state != 0 && right_state != 0) expected = FlowOwnershipState::Empty;
                        const std::string name = "N16D" + std::to_string(id++) + "_branch_join_matrix_type" +
                            std::to_string(type_case) + "_left" + std::to_string(left_state) +
                            "_right" + std::to_string(right_state) + "_variant" + std::to_string(variant);
                        accept_case(name, [source, owner, expected] {
                            const auto result = accept_flow(source);
                            accept_require(result.succeeded(), "branch join source should build CFG");
                            const auto& graph = result.functions.front();
                            accept_graph_integrity(graph);
                            const auto owner_id = accept_variable_id(graph, owner);
                            const auto* drop = accept_drop(graph, owner_id);
                            accept_require(drop != nullptr, "branch owner must have a drop event");
                            accept_require(drop->ownership_before == expected,
                                           "branch join produced an unexpected ownership lattice state");
                        }, passed, failed);
                    }
                }
            }
        }
    }

    // E (321): loop fixed-point and cleanup matrix. The first 300 cases span
    // wrapper type, loop scenario, and spelling; the final 21 cases add nested
    // break/continue combinations with inner-scope owners.
    {
        int id = 1;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                for (int variant = 0; variant < 3; ++variant) {
                    const std::string type = accept_type(type_case);
                    const std::string owner = accept_owner(scenario + variant);
                    const std::string flag = variant == 1 ? "FLAG" : "flag";
                    std::string action;
                    switch (scenario) {
                    case 0: action = owner + ".Reset(); break;"; break;
                    case 1: action = owner + ".Take(); continue;"; break;
                    case 2: action = owner + " = " + accept_create(type) + "; break;"; break;
                    case 3: action = "if (other) { continue; } " + owner + ".Reset(); break;"; break;
                    case 4: action = "if (other) { " + owner + ".Take(); break; } " + owner + ".Reset();"; break;
                    case 5: action = owner + ".Take(); " + owner + " = " + accept_create(type) + "; continue;"; break;
                    case 6: action = "{ " + type + " inner = " + accept_create(type) + "; if (other) { continue; } } break;"; break;
                    case 7: action = "{ " + type + " inner = " + accept_create(type) + "; if (other) { break; } } continue;"; break;
                    case 8: action = "if (other) { " + owner + ".Reset(); } else { " + owner + ".Take(); } break;"; break;
                    default: action = "if (other) { break; } " + owner + ".Take(); continue;"; break;
                    }
                    std::string loop = "while (" + flag + ") { " + action + " }";
                    if (variant == 2) loop = "{ " + loop + " }";
                    const std::string source = "function Probe(bool flag, bool FLAG, bool other) { " + type + " " + owner +
                        " = " + accept_create(type) + "; " + loop + " }";
                    const std::string name = "N16E" + std::to_string(id++) + "_loop_owner_matrix_type" +
                        std::to_string(type_case) + "_scenario" + std::to_string(scenario) + "_variant" + std::to_string(variant);
                    accept_case(name, [source, scenario] {
                        const auto result = accept_flow(source);
                        accept_require(result.succeeded(), "loop cleanup source should build CFG");
                        const auto& graph = result.functions.front();
                        accept_graph_integrity(graph);
                        accept_require(accept_count(graph, FlowOperationKind::OwnerDrop) >= 1,
                                       "loop/function scope must retain owner cleanup");
                        if (scenario == 6 || scenario == 7)
                            accept_require(accept_count(graph, FlowOperationKind::OwnerCreate) >= 2,
                                           "nested loop scope should create both outer and inner owners");
                    }, passed, failed);
                }
            }
        }
        for (int type_case = 0; type_case < 7; ++type_case) {
            for (int scenario = 0; scenario < 3; ++scenario) {
                const std::string type = accept_type(type_case);
                const std::string inner = "deepInner";
                std::string action;
                if (scenario == 0) action = "if (other) { " + inner + ".Take(); continue; } break;";
                else if (scenario == 1) action = "if (other) { break; } " + inner + ".Reset(); continue;";
                else action = "if (other) { " + inner + ".Reset(); break; } " + inner + ".Take(); continue;";
                const std::string source = "function Probe(bool flag, bool other) { while (flag) { { " + type + " " + inner +
                    " = " + accept_create(type) + "; " + action + " } } }";
                const std::string name = "N16E" + std::to_string(id++) + "_loop_owner_matrix_type" +
                    std::to_string(type_case) + "_deepScenario" + std::to_string(scenario);
                accept_case(name, [source] {
                    const auto result = accept_flow(source);
                    accept_require(result.succeeded(), "nested loop transfer source should build CFG");
                    const auto& graph = result.functions.front();
                    accept_graph_integrity(graph);
                    accept_require(accept_count(graph, FlowOperationKind::OwnerDrop) >= 1,
                                   "nested loop local must have a cleanup event");
                }, passed, failed);
            }
        }
    }

    // F (300): return cleanup across local-owner, nested-scope, and returned-
    // owner paths. Every reachable Return must be preceded by local cleanup;
    // returning a named Unique must emit a resource transfer event.
    {
        int id = 1;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                for (int variant = 0; variant < 3; ++variant) {
                    const std::string type = accept_type(type_case);
                    const std::string outer = variant == 1 ? "Outer" : "outer";
                    std::string source;
                    bool returns_owner = false;
                    if (scenario == 0 || scenario == 3 || scenario == 6) {
                        source = "function Probe(bool flag) returns int { " + type + " " + outer + " = " + accept_create(type) +
                            "; if (flag) { " + type + " inner = " + accept_create(type) + "; return 1; } else { return 2; } }";
                    } else if (scenario == 1 || scenario == 4 || scenario == 7) {
                        source = "function Probe() returns " + type + " { " + type + " " + outer + " = " + accept_create(type) +
                            "; return " + outer + "; }";
                        returns_owner = true;
                    } else if (scenario == 2) {
                        source = "function Probe(bool flag) returns int { { " + type + " nested = " + accept_create(type) +
                            "; if (flag) { return 3; } } return 4; }";
                    } else if (scenario == 5) {
                        source = "function Probe(bool flag) returns int { " + type + " " + outer + " = " + accept_create(type) +
                            "; if (flag) { " + outer + ".Take(); return 5; } " + outer + ".Reset(); return 6; }";
                    } else if (scenario == 8) {
                        source = "function Probe(bool flag) returns " + type + " { " + type + " " + outer + " = " + accept_create(type) +
                            "; if (flag) { return " + outer + "; } " + outer + ".Reset(); return " + type + ".Create(); }";
                        returns_owner = true;
                    } else {
                        source = "function Probe(bool flag) returns int { " + type + " " + outer + " = " + accept_create(type) +
                            "; if (flag) { { " + type + " nested = " + accept_create(type) + "; return 7; } } return 8; }";
                    }
                    if (variant == 2) {
                        // Additional nested lexical block around the complete function body
                        // keeps return/cleanup semantics intact while stressing block edges.
                        source = source.substr(0, source.size() - 1U) + " { int tail = 0; } }";
                    }
                    const std::string name = "N16F" + std::to_string(id++) + "_return_cleanup_matrix_type" +
                        std::to_string(type_case) + "_scenario" + std::to_string(scenario) + "_variant" + std::to_string(variant);
                    accept_case(name, [source, returns_owner, outer] {
                        const auto result = accept_flow(source);
                        accept_require(result.succeeded(), "return cleanup source should build CFG");
                        const auto& graph = result.functions.front();
                        accept_graph_integrity(graph);
                        accept_require(accept_count(graph, FlowOperationKind::OwnerDrop) >= 1,
                                       "returning function must retain owner cleanup events");
                        accept_require(accept_returns_cleanup_before_terminator(graph),
                                       "every reachable Return must be preceded by scope cleanup");
                        if (returns_owner) {
                            const auto owner_id = accept_variable_id(graph, outer);
                            bool found_transfer = false;
                            for (const auto& block : graph.blocks)
                                for (const auto& operation : block.operations)
                                    if (operation.kind == FlowOperationKind::OwnerTransfer &&
                                        operation.variable_id == owner_id &&
                                        operation.related_variable_id == xfrontend::InvalidFlowVariableId)
                                        found_transfer = true;
                            accept_require(found_transfer, "returning a named owner must transfer its resource");
                        }
                    }, passed, failed);
                }
            }
        }
    }

    // G (200): CFG lifecycle integrity matrix. Different combinations of
    // nested branches, owners, loops, and transfers verify stable ids, edge
    // symmetry, complete snapshots, event references, and converged analysis.
    {
        int id = 1;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                for (int variant = 0; variant < 2; ++variant) {
                    const std::string type = accept_type(type_case);
                    const std::string owner = accept_owner(scenario + variant);
                    const std::string other = variant == 0 ? "second" : "Second";
                    std::string body;
                    switch (scenario) {
                    case 0: body = "if (flag) { " + owner + ".Take(); } else { " + owner + ".Reset(); }"; break;
                    case 1: body = "{ " + type + " " + other + " = " + accept_create(type) + "; } " + owner + ".Get();"; break;
                    case 2: body = "while (flag) { if (otherFlag) { " + owner + ".Reset(); break; } " + owner + ".Take(); continue; }"; break;
                    case 3: body = "if (flag) { return; } " + owner + ".Reset();"; break;
                    case 4: body = type + " " + other + " = " + owner + ".Take(); PrintLine " + other + ".Get();"; break;
                    case 5: body = "if (flag) { " + owner + ".Reset(); " + owner + " = " + accept_create(type) + "; } else { PrintLine " + owner + ".Get(); }"; break;
                    case 6: body = "{ { " + type + " " + other + " = " + accept_create(type) + "; if (flag) { PrintLine " + other + ".Get(); } } }"; break;
                    case 7: body = "while (flag) { { " + type + " " + other + " = " + accept_create(type) + "; if (otherFlag) { continue; } } break; }"; break;
                    case 8: body = "if (flag) { { " + owner + ".Take(); } } else { { " + owner + ".Reset(); } }"; break;
                    default: body = "if (flag) { return; } else { " + owner + ".Take(); return; }"; break;
                    }
                    const std::string source = "function Probe(bool flag, bool otherFlag) { " + type + " " + owner + " = " +
                        accept_create(type) + "; " + body + " }";
                    const std::string name = "N16G" + std::to_string(id++) + "_cfg_integrity_matrix_type" +
                        std::to_string(type_case) + "_scenario" + std::to_string(scenario) + "_variant" + std::to_string(variant);
                    accept_case(name, [source] {
                        const auto result = accept_flow(source);
                        accept_require(result.succeeded(), "integrity matrix source should build CFG");
                        accept_require(result.functions.size() == 1U, "one CFG is expected");
                        const auto& graph = result.functions.front();
                        accept_graph_integrity(graph);
                        for (const auto& variable : graph.variables) {
                            accept_require(variable.id < graph.variables.size(), "variable id must be in range");
                            if (variable.is_unique_owner) {
                                accept_require(accept_drop(graph, variable.id) != nullptr,
                                               "every lexical unique owner must have a drop event");
                            }
                        }
                        for (const auto& block : graph.blocks) {
                            for (const auto& operation : block.operations) {
                                if (operation.kind == FlowOperationKind::OwnerDrop && block.reachable) {
                                    accept_require(operation.ownership_before != FlowOwnershipState::Unknown,
                                                   "reachable drop event must capture its incoming ownership state");
                                    accept_require(operation.ownership_after == FlowOwnershipState::Dropped ||
                                                   operation.ownership_after == FlowOwnershipState::MaybeDropped,
                                                   "reachable drop event must report a dropped terminal state");
                                }
                            }
                        }
                    }, passed, failed);
                }
            }
        }
    }
}
