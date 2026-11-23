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

void deep_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::ControlFlowResult deep_flow(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("deep ownership source parse failed: " +
            parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message +
            " | source=" + source);
    }
    return xfrontend::analyze_control_flow(parsed.unit);
}

void deep_case(const std::string& name, const std::function<void()>& body,
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

std::string deep_type(int index) {
    static const std::vector<std::string> names{
        "Unique[Device]", "UniqueArray[Device]", "Unique[Widget]", "UniqueArray[Widget]",
        "Unique[Buffer]", "UniqueArray[Buffer]", "UNIQUE[Device]", "Unique[Node]",
        "UniqueArray[Node]", "Unique[Resource]"
    };
    return names[static_cast<std::size_t>(index % static_cast<int>(names.size()))];
}

std::string deep_create(const std::string& type) { return type + ".Create()"; }

xfrontend::FlowVariableId deep_variable_id(const xfrontend::ControlFlowGraph& graph,
                                           const std::string& name) {
    for (const auto& variable : graph.variables) {
        std::string lhs = variable.name;
        std::string rhs = name;
        for (char& ch : lhs) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        for (char& ch : rhs) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (lhs == rhs && variable.is_unique_owner) return variable.id;
    }
    throw std::runtime_error("owner not found: " + name);
}

int deep_count(const xfrontend::ControlFlowGraph& graph, xfrontend::FlowOperationKind kind) {
    int count = 0;
    for (const auto& block : graph.blocks)
        for (const auto& operation : block.operations)
            if (operation.kind == kind) ++count;
    return count;
}

std::vector<std::string> deep_drop_order(const xfrontend::ControlFlowGraph& graph) {
    std::vector<std::string> names;
    for (const auto& block : graph.blocks)
        for (const auto& operation : block.operations)
            if (operation.kind == xfrontend::FlowOperationKind::OwnerDrop) names.push_back(operation.name);
    return names;
}

void validate_graph_shape(const xfrontend::ControlFlowGraph& graph) {
    deep_require(graph.ownership_analysis_converged, "ownership fixed point must converge");
    deep_require(graph.entry_block < graph.blocks.size(), "entry block must be valid");
    deep_require(graph.exit_block < graph.blocks.size(), "exit block must be valid");
    for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
        const auto& block = graph.blocks[index];
        deep_require(block.id == index, "block ids must match stable vector indices");
        if (block.reachable) {
            deep_require(block.ownership_in.size() == graph.variables.size(),
                         "reachable block ownership input size mismatch");
            deep_require(block.ownership_out.size() == graph.variables.size(),
                         "reachable block ownership output size mismatch");
        }
        for (const auto successor : block.successors) {
            deep_require(successor < graph.blocks.size(), "successor id out of range");
            const auto& predecessors = graph.blocks[successor].predecessors;
            deep_require(std::find(predecessors.begin(), predecessors.end(), block.id) != predecessors.end(),
                         "CFG successor/predecessor relation must be symmetric");
        }
        for (const auto predecessor : block.predecessors) {
            deep_require(predecessor < graph.blocks.size(), "predecessor id out of range");
            const auto& successors = graph.blocks[predecessor].successors;
            deep_require(std::find(successors.begin(), successors.end(), block.id) != successors.end(),
                         "CFG predecessor/successor relation must be symmetric");
        }
        for (const auto& operation : block.operations) {
            if (operation.variable_id != xfrontend::InvalidFlowVariableId)
                deep_require(operation.variable_id < graph.variables.size(), "operation variable id out of range");
            if (operation.related_variable_id != xfrontend::InvalidFlowVariableId)
                deep_require(operation.related_variable_id < graph.variables.size(), "related variable id out of range");
            if (operation.kind == xfrontend::FlowOperationKind::OwnerDrop)
                deep_require(operation.conditional, "scope drop must be conditional on live ownership");
        }
    }
}

} // namespace

void run_ownership_deep_matrix_tests(int& passed, int& failed) {
    using namespace xfrontend;

    // Z (100): source-to-destination Take transfer across wrapper spellings,
    // declaration/assignment forms, and nesting. Every case validates the
    // source-empty transition and the destination-aware transfer metadata.
    {
        int id = 1;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = deep_type(type_case);
                const std::string source_name = scenario % 2 == 0 ? "source" : "SoUrCe";
                const std::string target_name = scenario % 3 == 0 ? "target" : "TaRgEt";
                std::string body;
                switch (scenario) {
                case 0:
                    body = type + " " + target_name + " = " + source_name + ".Take();";
                    break;
                case 1:
                    body = type + " " + target_name + " = " + deep_create(type) + "; " + target_name + " = " + source_name + ".Take();";
                    break;
                case 2:
                    body = "{ " + type + " " + target_name + " = " + source_name + ".Take(); PrintLine " + target_name + ".Get(); }";
                    break;
                case 3:
                    body = source_name + " = " + source_name + ".Take();";
                    break;
                case 4:
                    body = type + " mid = " + source_name + ".Take(); " + type + " " + target_name + " = mid.Take();";
                    break;
                case 5:
                    body = "if (flag) { " + type + " " + target_name + " = " + source_name + ".Take(); } else { PrintLine " + source_name + ".Get(); }";
                    break;
                case 6:
                    body = type + " " + target_name + " = " + source_name + ".Take(); " + target_name + " = " + deep_create(type) + ";";
                    break;
                case 7:
                    body = type + " second = " + deep_create(type) + "; " + type + " " + target_name + " = " + source_name + ".Take(); second.Reset();";
                    break;
                case 8:
                    body = type + " " + target_name + " = " + deep_create(type) + "; if (flag) { " + target_name + " = " + source_name + ".Take(); }";
                    break;
                default:
                    body = "{ " + type + " mid = " + source_name + ".Take(); { " + type + " " + target_name + " = mid.Take(); } }";
                    break;
                }
                const std::string source = "function Probe(bool flag) { " + type + " " + source_name + " = " + deep_create(type) + "; " + body + " }";
                const std::string name = "Z" + std::to_string(id++) + "_transfer_generation_matrix_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                deep_case(name, [source, source_name, scenario] {
                    const auto result = deep_flow(source);
                    deep_require(result.succeeded(), "CFG rejected valid Take transfer");
                    const auto& graph = result.functions.at(0);
                    validate_graph_shape(graph);
                    const auto source_id = deep_variable_id(graph, source_name);
                    bool found_transfer = false;
                    bool found_source_empty = false;
                    for (const auto& block : graph.blocks) {
                        for (const auto& operation : block.operations) {
                            if (operation.kind != FlowOperationKind::OwnerTransfer ||
                                operation.variable_id != source_id) continue;
                            found_transfer = true;
                            if (operation.related_variable_id != InvalidFlowVariableId || scenario == 3) {
                                found_source_empty = operation.ownership_after == FlowOwnershipState::Empty ||
                                    (operation.related_variable_id == source_id &&
                                     operation.ownership_after == FlowOwnershipState::Live);
                            }
                        }
                    }
                    deep_require(found_transfer, "Take must produce an ownership transfer event");
                    deep_require(found_source_empty, "Take must empty source or preserve self-transfer semantics");
                }, passed, failed);
            }
        }
    }

    // AA (100): branch joins exercise Live/Empty/MaybeLive states with nested
    // conditions and loop exits. All snapshots must be present and well-formed.
    {
        int id = 101;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = deep_type(type_case);
                const std::string owner = scenario % 2 == 0 ? "owner" : "OwNeR";
                std::string body;
                switch (scenario) {
                case 0: body = "if (flag) { " + owner + ".Reset(); } else { PrintLine " + owner + ".Get(); }"; break;
                case 1: body = "if (flag) { " + owner + ".Take(); } else { " + owner + ".Reset(); }"; break;
                case 2: body = "if (flag) { " + owner + ".Reset(); " + owner + " = " + deep_create(type) + "; } else { PrintLine " + owner + ".Get(); }"; break;
                case 3: body = "if (flag) { if (other) { " + owner + ".Take(); } else { " + owner + ".Reset(); } } else { " + owner + " = " + deep_create(type) + "; }"; break;
                case 4: body = "if (true) { " + owner + ".Reset(); } else { PrintLine " + owner + ".Get(); }"; break;
                case 5: body = "if (false) { " + owner + ".Take(); } else { PrintLine " + owner + ".Get(); }"; break;
                case 6: body = "while (flag) { " + owner + ".Reset(); if (other) { break; } " + owner + " = " + deep_create(type) + "; }"; break;
                case 7: body = "while (flag) { if (other) { " + owner + ".Take(); continue; } break; }"; break;
                case 8: body = "if (flag) { return; } " + owner + ".Get();"; break;
                default: body = "if (flag) { " + owner + ".Take(); } else { " + owner + " = " + deep_create(type) + "; }"; break;
                }
                const std::string source = "function Probe(bool flag, bool other) { " + type + " " + owner + " = " + deep_create(type) + "; " + body + " }";
                const std::string name = "AA" + std::to_string(id++) + "_flow_join_matrix_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                deep_case(name, [source] {
                    const auto result = deep_flow(source);
                    deep_require(result.succeeded(), "CFG rejected branch/loop ownership source");
                    deep_require(result.functions.size() == 1U, "expected one function graph");
                    validate_graph_shape(result.functions.front());
                    deep_require(result.functions.front().ownership_analysis_iterations > 0U,
                                 "ownership pass must execute to fixed point");
                }, passed, failed);
            }
        }
    }

    // AB (100): nested lexical owner scopes validate reverse-order drops,
    // declaration identity, and exact one-drop-per-local accounting.
    {
        int id = 201;
        for (int depth_case = 0; depth_case < 10; ++depth_case) {
            for (int owner_case = 0; owner_case < 10; ++owner_case) {
                const int depth = 1 + (depth_case % 5);
                const int owners_per_scope = 1 + (owner_case % 3);
                std::string source = "function Probe() { ";
                std::vector<std::string> declared;
                for (int scope = 0; scope < depth; ++scope) {
                    source += "{ ";
                    for (int owner = 0; owner < owners_per_scope; ++owner) {
                        const std::string name = "owner" + std::to_string(scope) + "_" + std::to_string(owner);
                        const std::string type = deep_type(depth_case + owner_case + scope + owner);
                        declared.push_back(name);
                        source += type + " " + name + " = " + deep_create(type) + "; ";
                    }
                }
                for (int scope = 0; scope < depth; ++scope) source += "} ";
                source += "}";
                const std::string name = "AB" + std::to_string(id++) + "_scope_order_matrix_depth" + std::to_string(depth) + "_owners" + std::to_string(owners_per_scope);
                deep_case(name, [source, declared] {
                    const auto result = deep_flow(source);
                    deep_require(result.succeeded(), "nested owner scopes should build CFG");
                    const auto& graph = result.functions.at(0);
                    validate_graph_shape(graph);
                    const auto drops = deep_drop_order(graph);
                    deep_require(drops.size() == declared.size(), "each local Unique must have one lexical drop event");
                    for (std::size_t index = 0; index < declared.size(); ++index) {
                        deep_require(drops[index] == declared[declared.size() - index - 1U],
                                     "nested scopes must drop in reverse declaration order");
                    }
                }, passed, failed);
            }
        }
    }

    // AC (100): loop-local owners on normal, break, and continue paths must
    // retain cleanup and keep CFG predecessor/successor links symmetric.
    {
        int id = 301;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = deep_type(type_case);
                const std::string owner = scenario % 2 == 0 ? "loopOwner" : "LoopOwner";
                std::string action;
                switch (scenario) {
                case 0: action = owner + ".Reset(); break;"; break;
                case 1: action = owner + ".Take(); continue;"; break;
                case 2: action = owner + " = " + deep_create(type) + "; break;"; break;
                case 3: action = "if (inner) { continue; } " + owner + ".Reset(); break;"; break;
                case 4: action = "if (inner) { " + owner + ".Take(); break; } " + owner + ".Reset();"; break;
                case 5: action = owner + ".Take(); " + owner + " = " + deep_create(type) + "; continue;"; break;
                case 6: action = "if (inner) { " + owner + ".Reset(); } else { " + owner + ".Take(); } break;"; break;
                case 7: action = owner + ".Reset(); if (inner) { break; } continue;"; break;
                case 8: action = "if (inner) { " + owner + ".Take(); } break;"; break;
                default: action = "if (inner) { continue; } else { break; }"; break;
                }
                const std::string source = "function Probe(bool flag, bool inner) { while (flag) { " + type + " " + owner + " = " + deep_create(type) + "; " + action + " } }";
                const std::string name = "AC" + std::to_string(id++) + "_loop_owner_matrix_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                deep_case(name, [source] {
                    const auto result = deep_flow(source);
                    deep_require(result.succeeded(), "loop cleanup source should build CFG");
                    const auto& graph = result.functions.at(0);
                    validate_graph_shape(graph);
                    deep_require(deep_count(graph, FlowOperationKind::OwnerDrop) >= 1,
                                 "loop-local owner must have at least one cleanup event");
                    deep_require(graph.blocks.size() >= 5U, "loop CFG must contain distinct control-flow blocks");
                }, passed, failed);
            }
        }
    }

    // AD (100): event and snapshot integrity under repeated reset/create/take
    // sequences, with several owners and mixed identifier casing.
    {
        int id = 401;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = deep_type(type_case);
                const std::string first = scenario % 2 == 0 ? "first" : "FiRsT";
                const std::string second = scenario % 3 == 0 ? "second" : "SeCoNd";
                const std::string third = scenario % 4 == 0 ? "third" : "ThIrD";
                std::string sequence;
                switch (scenario) {
                case 0: sequence = first + ".Reset(); " + first + " = " + deep_create(type) + ";"; break;
                case 1: sequence = second + " = " + first + ".Take(); " + second + ".Reset();"; break;
                case 2: sequence = first + ".Take(); " + first + " = " + deep_create(type) + "; " + first + ".Get();"; break;
                case 3: sequence = third + " = " + second + ".Take(); " + second + " = " + first + ".Take();"; break;
                case 4: sequence = "if (flag) { " + first + ".Reset(); } else { " + first + ".Take(); } " + first + " = " + deep_create(type) + ";"; break;
                case 5: sequence = first + ".Reset(); " + second + " = " + first + ".Take(); " + second + " = " + deep_create(type) + ";"; break;
                case 6: sequence = first + " = " + first + ".Take(); " + first + ".Reset(); " + first + " = " + deep_create(type) + ";"; break;
                case 7: sequence = second + " = " + first + ".Take(); " + third + " = " + second + ".Take(); " + third + ".Get();"; break;
                case 8: sequence = "if (flag) { " + second + " = " + first + ".Take(); } " + first + " = " + deep_create(type) + ";"; break;
                default: sequence = first + ".Take(); " + second + " = " + deep_create(type) + "; " + third + " = " + second + ".Take();"; break;
                }
                const std::string source = "function Probe(bool flag) { " + type + " " + first + " = " + deep_create(type) + "; " + type + " " + second + " = " + deep_create(type) + "; " + type + " " + third + " = " + deep_create(type) + "; " + sequence + " }";
                const std::string name = "AD" + std::to_string(id++) + "_cfg_integrity_matrix_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                deep_case(name, [source] {
                    const auto result = deep_flow(source);
                    deep_require(result.succeeded(), "owner lifecycle sequence should build CFG");
                    const auto& graph = result.functions.at(0);
                    validate_graph_shape(graph);
                    deep_require(deep_count(graph, FlowOperationKind::OwnerCreate) >= 3,
                                 "all initialized owners must have creation events");
                    deep_require(deep_count(graph, FlowOperationKind::OwnerDrop) >= 3,
                                 "all function-scope owners must have cleanup events");
                }, passed, failed);
            }
        }
    }
}
